#include "chunkharbor/net/TcpConnection.h"
#include "chunkharbor/net/EventLoop.h"
#include <algorithm>
#include <cerrno>
#include <stdexcept>
#include <sys/socket.h>

namespace chunkharbor {
namespace { constexpr std::size_t IoBudget = 256 * 1024; }
TcpConnection::TcpConnection(EventLoop& loop, UniqueFd fd, Limits limits)
    : loop_(loop), fd_(std::move(fd)), channel_(loop, fd_.get()), limits_(limits),
      input_(std::min<std::size_t>(1024, limits.inputLimit), limits.inputLimit),
      output_(std::min<std::size_t>(1024, limits.outputLimit), limits.outputLimit) {
    if (!fd_ || limits.lowWater >= limits.highWater || limits.highWater > limits.outputLimit)
        throw std::invalid_argument("invalid TCP limits");
    channel_.setReadCallback([this] { readReady(); });
    channel_.setWriteCallback([this] { writeReady(); });
    channel_.setErrorCallback([this] { close(); });
}
void TcpConnection::start() {
    loop_.assertInLoopThread();
    if (started_ || closed_) throw std::logic_error("connection already started or closed");
    channel_.tie(shared_from_this()); started_ = true; refreshInterest();
}
void TcpConnection::refreshInterest() {
    if (closed_) return;
    if (output_.readableBytes() >= limits_.highWater) readPaused_ = true;
    else if (output_.readableBytes() <= limits_.lowWater) readPaused_ = false;
    channel_.setInterest(!readEof_ && !readPaused_, output_.readableBytes() != 0);
}
void TcpConnection::send(std::string_view bytes) {
    loop_.assertInLoopThread();
    if (closed_) return;
    if (!started_) throw std::logic_error("send before connection start");
    if (bytes.size() > limits_.outputLimit - output_.readableBytes()) { close(); return; }
    output_.append(bytes); writeReady();
}
void TcpConnection::writeReady() {
    std::size_t budget = IoBudget;
    while (!closed_ && output_.readableBytes() && budget) {
        const auto view = output_.readableView();
        const auto requested = std::min(view.size(), budget);
        const auto n = ::send(fd_.get(), view.data(), requested, MSG_NOSIGNAL);
        if (n > 0) {
            if (static_cast<std::size_t>(n) < requested) ++stats_.shortWrites;
            output_.consume(n); budget -= n; lastActivity_ = std::chrono::steady_clock::now();
        } else if (n < 0 && errno == EINTR) { ++stats_.interruptions; continue; }
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { ++stats_.wouldBlockWrites; break; }
        else { close(); return; }
    }
    if (readEof_ && output_.readableBytes() == 0) { close(); return; }
    refreshInterest();
}
void TcpConnection::readReady() {
    std::size_t budget = IoBudget;
    while (!closed_ && !readEof_ && !readPaused_ && budget) {
        const auto space = std::min({std::size_t(64 * 1024), budget,
                                    limits_.inputLimit - input_.readableBytes()});
        if (!space) { close(); return; }
        input_.ensureWritable(space);
        const auto n = ::recv(fd_.get(), input_.writableData(), space, 0);
        if (n > 0) {
            input_.commitWrite(n); budget -= n; lastActivity_ = std::chrono::steady_clock::now();
            if (messageCallback_) messageCallback_(shared_from_this(), input_);
        } else if (n == 0) {
            readEof_ = true;
            if (input_.readableBytes() || !output_.readableBytes()) close();
            else refreshInterest();
        } else if (errno == EINTR) { ++stats_.interruptions; continue; }
        else if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        else { close(); return; }
    }
}
void TcpConnection::close() {
    loop_.assertInLoopThread();
    if (closed_) return;
    const auto guard = shared_from_this();
    closed_ = true; channel_.remove(); ::shutdown(fd_.get(), SHUT_RDWR);
    if (closeCallback_) closeCallback_(guard);
}
} // namespace chunkharbor
