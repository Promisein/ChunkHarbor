#include "DownloadClient.h"
#include "chunkharbor/client_core/HandshakeExchange.h"
#include "chunkharbor/common/Sha256.h"
#include "chunkharbor/common/UniqueFd.h"
#include "chunkharbor/protocol/Download.h"
#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <set>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>

namespace {
using namespace chunkharbor;
using namespace chunkharbor::protocol;
using Clock = std::chrono::steady_clock;
void require(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}
void waitFor(int fd, short events, Clock::time_point deadline) {
    for (;;) {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        require(left > 0, "download timeout");
        pollfd p{fd, events, 0};
        auto n = ::poll(&p, 1, static_cast<int>(left));
        if (n < 0 && errno == EINTR) continue;
        require(n > 0, "download poll failed or timed out");
        return;
    }
}
// One bounded response per pull request, including partial frames and writes.
class Connection {
    UniqueFd fd_;
    std::string input_;
  public:
    explicit Connection(std::uint16_t port)
        : fd_(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)) {
        require(bool(fd_), "download socket failed");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd_.get(), reinterpret_cast<sockaddr *>(&address), sizeof address)) {
            require(errno == EINPROGRESS || errno == EINTR, "download connect failed");
            waitFor(fd_.get(), POLLOUT, Clock::now() + std::chrono::seconds(5));
            int error = 0;
            socklen_t length = sizeof error;
            require(::getsockopt(fd_.get(), SOL_SOCKET, SO_ERROR, &error, &length) == 0 && !error,
                    "download connect failed");
        }
    }
    Frame exchange(const Frame &request, std::uint16_t expected) {
        auto deadline = Clock::now() + std::chrono::seconds(30);
        auto bytes = encode(request);
        std::size_t sent = 0;
        while (sent < bytes.size()) {
            require(Clock::now() < deadline, "download timeout");
            auto n = ::send(fd_.get(), bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
            if (n > 0) sent += static_cast<std::size_t>(n);
            else if (n < 0 && errno == EINTR) continue;
            else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) waitFor(fd_.get(), POLLOUT, deadline);
            else throw std::runtime_error("download send failed");
        }
        const std::size_t capacity = HeaderSize + (expected == 14
            ? DataChunkHeaderSize + MaxDownloadChunkSize : MaxControlPayload);
        for (;;) {
            require(Clock::now() < deadline, "download timeout");
            auto decoded = decode(input_);
            require(decoded.status != DecodeStatus::Error, "invalid download frame");
            if (decoded.status == DecodeStatus::Complete) {
                auto response = std::move(decoded.frame);
                input_.erase(0, decoded.consumed);
                require(response.requestId == request.requestId, "download response request mismatch");
                if (response.type == 15) {
                    auto error = decodeUploadError(response.payload);
                    throw UploadFailure(error.code, error.message);
                }
                require(response.type == expected, "unexpected download response type");
                return response;
            }
            require(input_.size() < capacity, "download response exceeds limit");
            char buffer[8192];
            auto n = ::recv(fd_.get(), buffer, std::min(sizeof buffer, capacity - input_.size()), 0);
            if (n > 0) input_.append(buffer, static_cast<std::size_t>(n));
            else if (n < 0 && errno == EINTR) continue;
            else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) waitFor(fd_.get(), POLLIN, deadline);
            else throw std::runtime_error("server closed before download response");
        }
    }
};
void syncFd(int fd, const char *message) {
    int result;
    do { result = ::fsync(fd); } while (result < 0 && errno == EINTR);
    require(result == 0, message);
}
class TempOutput {
    UniqueFd directory_, file_;
    std::string target_, temporary_;
  public:
    explicit TempOutput(const std::string &output) {
        std::filesystem::path path(output);
        target_ = path.filename().string();
        require(!target_.empty() && target_ != "." && target_ != "..", "invalid output filename");
        directory_.reset(::open(path.is_absolute() ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        require(bool(directory_), "cannot open output directory");
        for (const auto &component : path.parent_path()) {
            auto name = component.string();
            if (name.empty() || name == "/" || name == ".") continue;
            require(name != "..", "output parent traversal is not allowed");
            UniqueFd next(::openat(directory_.get(), name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            require(bool(next), "output parent must exist without symlinks");
            directory_ = std::move(next);
        }
        struct stat st{};
        require(::fstatat(directory_.get(), target_.c_str(), &st, AT_SYMLINK_NOFOLLOW) < 0 && errno == ENOENT,
                "output already exists or cannot be inspected");
        // The only exception-capable operations after creation are handled by the
        // completed object's destructor; failed O_EXCL never owns another file.
        std::array<unsigned char, 16> random{};
        std::size_t done = 0;
        while (done < random.size()) {
            auto n = ::getrandom(random.data() + done, random.size() - done, 0);
            if (n < 0 && errno == EINTR) continue;
            require(n > 0, "cannot generate temporary filename");
            done += static_cast<std::size_t>(n);
        }
        temporary_ = ".chunkharbor-download-";
        for (auto b : random) {
            temporary_ += "0123456789abcdef"[b >> 4];
            temporary_ += "0123456789abcdef"[b & 15];
        }
        file_.reset(::openat(directory_.get(), temporary_.c_str(),
                             O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
        require(bool(file_), "cannot create download temporary file");
    }
    ~TempOutput() {
        if (!temporary_.empty()) ::unlinkat(directory_.get(), temporary_.c_str(), 0);
    }
    void write(std::string_view bytes) {
        while (!bytes.empty()) {
            auto n = ::write(file_.get(), bytes.data(), bytes.size());
            if (n < 0 && errno == EINTR) continue;
            require(n > 0, "download temporary write failed");
            bytes.remove_prefix(static_cast<std::size_t>(n));
        }
    }
    void publish() {
        syncFd(file_.get(), "download temporary fsync failed");
        long result;
        do {
            result = ::syscall(SYS_renameat2, directory_.get(), temporary_.c_str(),
                               directory_.get(), target_.c_str(), RENAME_NOREPLACE);
        } while (result < 0 && errno == EINTR);
        require(result == 0, "download publication failed; target may already exist");
        temporary_.clear();
        syncFd(directory_.get(), "output published but directory fsync failed");
    }
};
} // namespace
int runDownloadCommand(int argc, char **argv) {
    std::string path, output, version;
    unsigned port = 8787;
    std::set<std::string> seen;
    for (int i = 2; i < argc; i += 2) {
        require(i + 1 < argc, "missing download option value");
        std::string key(argv[i]), value(argv[i + 1]);
        require(seen.insert(key).second, "duplicate download option");
        if (key == "--path") path = value;
        else if (key == "--output") output = value;
        else if (key == "--version-id") version = value;
        else if (key == "--host") require(value == "127.0.0.1", "download uses trusted loopback/SSH only");
        else if (key == "--port") {
            auto parsed = std::from_chars(value.data(), value.data() + value.size(), port);
            require(parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && port && port <= 65535,
                    "invalid download port");
        } else throw std::runtime_error("unknown download option");
    }
    require(!path.empty() && !output.empty(), "download requires --path and --output");
    BeginDownload begin{path, version};
    auto body = encodeBeginDownload(begin);
    TempOutput destination(output);
    Connection connection(static_cast<std::uint16_t>(port));
    Hello offer;
    offer.capabilities = {"handshake", "download_v1"};
    client::HandshakeExchange handshake(offer, 1);
    handshake.accept(connection.exchange(handshake.request(), 2));
    const auto &ack = handshake.ack();
    require(std::find(ack.capabilities.begin(), ack.capabilities.end(), "download_v1") != ack.capabilities.end(),
            "server does not offer download_v1");
    auto ready = decodeDownloadReady(connection.exchange({13, 2, std::move(body)}, 32).payload);
    require(ready.path == path && (version.empty() || ready.versionId == version), "download metadata mismatch");
    Sha256 hash;
    std::uint64_t offset = 0, requestId = 3;
    const auto maxData = ack.maxDataPayload > DataChunkHeaderSize
        ? std::min<std::uint64_t>(MaxDownloadChunkSize, ack.maxDataPayload - DataChunkHeaderSize) : 0;
    require(ready.size == 0 || maxData > 0, "server download data limit is too small");
    while (offset < ready.size) {
        auto length = static_cast<std::uint32_t>(std::min<std::uint64_t>(maxData, ready.size - offset));
        auto chunk = decodeDownloadChunk(connection.exchange(
            {33, requestId++, encodeGetDownloadChunk({ready.versionId, offset, length})}, 14).payload);
        require(chunk.versionId == ready.versionId && chunk.offset == offset && chunk.data.size() == length,
                "download chunk identity or range mismatch");
        require(chunk.sha256 == sha256(chunk.data), "download chunk checksum mismatch");
        destination.write(chunk.data);
        hash.update(chunk.data);
        offset += chunk.data.size();
    }
    require(toHex(hash.finish()) == ready.sha256, "download whole-file checksum mismatch");
    destination.publish();
    std::cout << "DOWNLOAD OK: size=" << ready.size << " sha256=" << ready.sha256 << '\n';
    return 0;
}
