#include "UploadClient.h"
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
struct stat status(int fd) {
    struct stat s{};
    if (::fstat(fd, &s))
        throw std::runtime_error("source stat failed");
    return s;
}
bool same(const struct stat &a, const struct stat &b) {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size &&
           a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
           a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}
void stable(int fd, const struct stat &original) {
    if (!same(original, status(fd)))
        throw UploadFailure("SOURCE_CHANGED", "Source changed during upload");
}
std::string readAt(int fd, std::uint64_t offset, size_t count) {
    std::string b(count, '\0');
    size_t done = 0;
    while (done < count) {
        auto n = ::pread(fd, b.data() + done, count - done, static_cast<off_t>(offset + done));
        if (n > 0)
            done += static_cast<size_t>(n);
        else if (n < 0 && errno == EINTR)
            continue;
        else
            throw UploadFailure("SOURCE_CHANGED", "Source read failed or changed");
    }
    return b;
}
Frame response(Connection &c, std::uint16_t type, std::uint64_t id, std::string body,
               std::uint16_t expected, const std::function<void()> &onSent = {}) {
    auto f = c.exchange({type, id, std::move(body)}, onSent);
    if (f.type != expected)
        throw std::runtime_error("unexpected response type");
    return f;
}
} // namespace
void runUpload(const std::string &source, const std::string &path, const std::string &host,
               std::uint16_t port, std::uint32_t chunkSize) {
    using namespace chunkharbor;
    using namespace chunkharbor::protocol;
    if (host != "127.0.0.1")
        throw std::invalid_argument("upload uses trusted loopback or SSH tunnel only");
    if (chunkSize == 0 || chunkSize > MaxChunkDataSize)
        throw std::invalid_argument("invalid chunk size");
    UniqueFd fd(::open(source.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    if (!fd)
        throw std::runtime_error("cannot open source");
    auto original = status(fd.get());
    if (!S_ISREG(original.st_mode) || original.st_size < 0 || original.st_size > 64 * 1024 * 1024)
        throw std::runtime_error("source must be regular and at most 64 MiB");
    auto size = static_cast<std::uint64_t>(original.st_size);
    if ((size + chunkSize - 1) / chunkSize > 65536)
        throw std::runtime_error("too many blocks");
    Sha256 hash;
    for (std::uint64_t offset = 0; offset < size;) {
        auto b = readAt(fd.get(), offset,
                        static_cast<size_t>(std::min<std::uint64_t>(65536, size - offset)));
        hash.update(b);
        offset += b.size();
    }
    stable(fd.get(), original);
    constexpr std::int64_t billion = 1000000000;
    const auto seconds = original.st_mtim.tv_sec;
    const auto nanos = original.st_mtim.tv_nsec;
    constexpr auto min = std::numeric_limits<std::int64_t>::min();
    constexpr auto max = std::numeric_limits<std::int64_t>::max();
    if (nanos < 0 || nanos >= billion || seconds < min / billion - 1 || seconds > max / billion ||
        (seconds == min / billion - 1 && nanos < billion + min % billion) ||
        (seconds == max / billion && nanos > max % billion))
        throw std::runtime_error("source modification time cannot be represented");
    // For negative seconds, avoid multiplying the floor-second below INT64_MIN.
    const std::int64_t mtime =
        seconds < 0 ? static_cast<std::int64_t>(seconds + 1) * billion + (nanos - billion)
                    : static_cast<std::int64_t>(seconds) * billion + nanos;
    BeginUpload begin{path, size, mtime, chunkSize, toHex(hash.finish())};
    auto body = encodeBeginUpload(begin);
    Connection c(host, port);
    Hello offer;
    offer.capabilities = {"handshake", "upload"};
    client::HandshakeExchange handshake(offer, 1);
    handshake.accept(c.exchange(handshake.request()));
    if (std::find(handshake.ack().capabilities.begin(), handshake.ack().capabilities.end(),
                  "upload") == handshake.ack().capabilities.end())
        throw std::runtime_error("server lacks upload capability");
    if (body.size() > handshake.ack().maxControlPayload ||
        DataChunkHeaderSize + std::min<std::uint64_t>(size, chunkSize) >
            handshake.ack().maxDataPayload)
        throw std::runtime_error("server negotiated insufficient message limits");
    std::uint64_t id = 2;
    auto plan = decodeUploadPlan(response(c, 3, id++, body, 4).payload);
    validateUploadPlanForFile(plan, size, chunkSize);
    std::uint64_t sent = 0, confirmed = 0;
    for (auto range : plan.missingRanges)
        for (auto block = range.begin; block < range.end; ++block) {
            stable(fd.get(), original);
            auto offset = block * chunkSize;
            auto data =
                readAt(fd.get(), offset,
                       static_cast<size_t>(std::min<std::uint64_t>(chunkSize, size - offset)));
            DataChunk chunk{plan.sessionId, offset, sha256(data), std::move(data)};
            auto encoded = encodeDataChunk(chunk);
            auto ack = decodeChunkAck(response(c, 5, id++, std::move(encoded), 6, [&] {
                                          sent += chunk.data.size();
                                          std::cout << "sent=" << sent << " confirmed=" << confirmed
                                                    << '\n';
                                      }).payload);
            validateChunkAckForChunk(ack, chunk);
            confirmed += chunk.data.size();
            std::cout << "sent=" << sent << " confirmed=" << confirmed << '\n';
        }
    stable(fd.get(), original);
    auto result =
        decodeCommitResult(response(c, 9, id++, encodeCommitUpload({plan.sessionId}), 10).payload);
    if (result.sessionId != plan.sessionId || result.path != path || result.size != size ||
        result.sha256 != begin.sha256)
        throw std::runtime_error("commit result mismatch");
    stable(fd.get(), original);
    std::cout << "UPLOAD OK path=" << path << " size=" << size << " sha256=" << begin.sha256
              << '\n';
}
