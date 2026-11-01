#include "DeltaClient.h"
#include "chunkharbor/client_core/HandshakeExchange.h"
#include "chunkharbor/common/Sha256.h"
#include "chunkharbor/common/UniqueFd.h"
#include "chunkharbor/protocol/ChunkAck.h"
#include "chunkharbor/protocol/UploadControl.h"
#include "chunkharbor/protocol/UploadPlan.h"
#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <limits>
#include <poll.h>
#include <random>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
namespace {
using namespace chunkharbor;
using namespace chunkharbor::protocol;
using Clock = std::chrono::steady_clock;
void waitFor(int fd, short events, Clock::time_point until) {
    for (;;) {
        auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now()).count();
        if (left <= 0)
            throw std::runtime_error("upload timeout");
        pollfd p{fd, events, 0};
        auto n = ::poll(&p, 1, static_cast<int>(left));
        if (n > 0)
            return;
        if (n < 0 && errno == EINTR)
            continue;
        throw std::runtime_error("upload poll failed or timed out");
    }
}
class Connection {
    UniqueFd fd_;
    std::string input_;

  public:
    Connection(const std::string &host, std::uint16_t port)
        : fd_(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)) {
        if (!fd_)
            throw std::runtime_error("socket failed");
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        if (::inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1)
            throw std::runtime_error("invalid IPv4 host");
        if (::connect(fd_.get(), reinterpret_cast<sockaddr *>(&a), sizeof(a))) {
            if (errno != EINPROGRESS && errno != EINTR)
                throw std::runtime_error("connect failed");
            waitFor(fd_.get(), POLLOUT, Clock::now() + std::chrono::seconds(5));
            int e = 0;
            socklen_t n = sizeof(e);
            if (::getsockopt(fd_.get(), SOL_SOCKET, SO_ERROR, &e, &n) || e)
                throw std::runtime_error("connect failed");
        }
    }
    Frame exchange(const Frame &request, const std::function<void()> &onSent = {}) {
        const auto until = Clock::now() + std::chrono::seconds(30);
        auto wire = encode(request);
        size_t sent = 0;
        while (sent < wire.size()) {
            if (Clock::now() >= until)
                throw std::runtime_error("upload timeout");
            auto n = ::send(fd_.get(), wire.data() + sent, wire.size() - sent, MSG_NOSIGNAL);
            if (n > 0)
                sent += static_cast<size_t>(n);
            else if (n < 0 && errno == EINTR)
                continue;
            else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                waitFor(fd_.get(), POLLOUT, until);
            else
                throw std::runtime_error("send failed");
        }
        if (onSent)
            onSent();
        for (;;) {
            if (Clock::now() >= until)
                throw std::runtime_error("upload timeout");
            auto result = decode(input_);
            if (result.status == DecodeStatus::Error)
                throw std::runtime_error("invalid server frame");
            if (result.status == DecodeStatus::Complete) {
                input_.erase(0, result.consumed);
                if (result.frame.requestId != request.requestId)
                    throw std::runtime_error("response request mismatch");
                if (result.frame.type == 15) {
                    auto e = decodeUploadError(result.frame.payload);
                    throw UploadFailure(e.code, e.message);
                }
                return result.frame;
            }
            if (input_.size() >= HeaderSize + MaxControlPayload)
                throw std::runtime_error("response exceeds control limit");
            char b[8192];
            auto n = ::recv(fd_.get(), b,
                            std::min(sizeof(b), HeaderSize + MaxControlPayload - input_.size()), 0);
            if (n > 0)
                input_.append(b, static_cast<size_t>(n));
            else if (n < 0 && errno == EINTR)
                continue;
            else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                waitFor(fd_.get(), POLLIN, until);
            else
                throw std::runtime_error("server closed before response");
        }
    }
};

UniqueFd safeOpenSource(const std::filesystem::path &source) {
    auto absolute = std::filesystem::absolute(source).lexically_normal();
    UniqueFd directory(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    auto relative = absolute.relative_path();
    for (auto it = relative.begin(); it != relative.end(); ++it) {
        auto next = it;
        ++next;
        auto name = it->string();
        // A raced-in FIFO must reach fstat and be rejected, never block open().
        int flags = O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK;
        if (next != relative.end())
            flags |= O_DIRECTORY;
        UniqueFd opened(::openat(directory.get(), name.c_str(), flags));
        if (!opened)
            throw std::runtime_error("source path link or open failure");
        directory = std::move(opened);
    }
    return directory;
}
struct Owned {
    Connection connection;
    UniqueFd fd;
    struct stat identity{};
    Owned(const std::filesystem::path &path, const std::string &host, std::uint16_t port)
        : connection(host, port), fd(safeOpenSource(path)) {
        if (!fd || ::fstat(fd.get(), &identity) || !S_ISREG(identity.st_mode))
            throw std::runtime_error("source must be a regular non-link file");
    }
    std::string read(std::uint64_t offset, std::size_t length) {
        std::string bytes(length, '\0');
        std::size_t done = 0;
        while (done < length) {
            auto n = ::pread(fd.get(), bytes.data() + done, length - done,
                             static_cast<off_t>(offset + done));
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                throw std::runtime_error("source read failed");
            done += static_cast<std::size_t>(n);
        }
        return bytes;
    }
    std::string digest() {
        Sha256 hash;
        for (std::uint64_t offset = 0; offset < static_cast<std::uint64_t>(identity.st_size);) {
            auto bytes =
                read(offset, static_cast<std::size_t>(std::min<std::uint64_t>(
                                 65536, static_cast<std::uint64_t>(identity.st_size) - offset)));
            hash.update(bytes);
            offset += bytes.size();
        }
        return toHex(hash.finish());
    }
};
} // namespace
DeltaPlatform openDeltaPlatform(const std::filesystem::path &file, const std::string &host,
                                std::uint16_t port, std::uint32_t chunk,
                                const std::string &logical) {
    auto owned = std::make_shared<Owned>(file, host, port);
    Hello hello;
    hello.capabilities = {"handshake", "upload", "fixed_block_delta_v1"};
    client::HandshakeExchange hs(hello, 1);
    hs.accept(owned->connection.exchange(hs.request()));
    if (std::find(hs.ack().capabilities.begin(), hs.ack().capabilities.end(),
                  "fixed_block_delta_v1") == hs.ack().capabilities.end())
        throw std::runtime_error("server does not offer delta capability");
    if (owned->identity.st_size < 0 ||
        static_cast<std::uint64_t>(owned->identity.st_size) > 64ULL * 1024 * 1024 || !chunk ||
        (static_cast<std::uint64_t>(owned->identity.st_size) + chunk - 1) / chunk > 65536)
        throw std::runtime_error("invalid source geometry");
    auto digest = owned->digest();
    BeginUpload m{logical, static_cast<std::uint64_t>(owned->identity.st_size),
                  static_cast<std::int64_t>(owned->identity.st_mtim.tv_sec) * 1000000000 +
                      owned->identity.st_mtim.tv_nsec,
                  chunk, digest};
    (void)encodeBeginUpload(m);
    client::SourceSnapshot source{
        m, [owned](auto offset, auto count) { return owned->read(offset, count); },
        [](auto bytes) { return sha256(bytes); },
        [owned, digest] {
            struct stat now{};
            if (::fstat(owned->fd.get(), &now) || now.st_dev != owned->identity.st_dev ||
                now.st_ino != owned->identity.st_ino || now.st_size != owned->identity.st_size ||
                now.st_mtim.tv_sec != owned->identity.st_mtim.tv_sec ||
                now.st_mtim.tv_nsec != owned->identity.st_mtim.tv_nsec || owned->digest() != digest)
                throw std::runtime_error("source changed");
        }};
    return {std::move(source),
            [owned](const Frame &frame) { return owned->connection.exchange(frame); }, owned};
}
void atomicDeltaState(const std::filesystem::path &path, const std::string &value) {
    auto temp = path;
    temp += ".tmp." + std::to_string(std::random_device{}());
    UniqueFd fd(::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (!fd)
        throw std::runtime_error("cannot create state temporary file");
    try {
        std::size_t written = 0;
        while (written < value.size()) {
            auto n = ::write(fd.get(), value.data() + written, value.size() - written);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                throw std::runtime_error("state write failed");
            written += static_cast<std::size_t>(n);
        }
        if (::fsync(fd.get()) || ::rename(temp.c_str(), path.c_str()))
            throw std::runtime_error("state publication failed");
        auto parent = path.parent_path();
        if (parent.empty())
            parent = ".";
        UniqueFd directory(::open(parent.c_str(), O_DIRECTORY | O_RDONLY | O_CLOEXEC));
        if (!directory || ::fsync(directory.get()))
            throw std::runtime_error("state directory sync failed");
    } catch (...) {
        ::unlink(temp.c_str());
        throw;
    }
}
