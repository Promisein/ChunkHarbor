#include "chunkharbor/net/TcpServer.h"
#include "chunkharbor/common/Error.h"
#include "chunkharbor/net/EventLoop.h"
#include <arpa/inet.h>
#include <cerrno>
#include <stdexcept>
#include <sys/timerfd.h>
#include <unistd.h>
#include <vector>

namespace chunkharbor {
TcpServer::TcpServer(EventLoop& loop, std::uint16_t port, std::chrono::milliseconds idleTimeout,
                     std::size_t maxConnections)
    : loop_(loop), listener_(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)),
      timer_(::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC)),
      acceptChannel_(loop, listener_.get()), timerChannel_(loop, timer_.get()),
      idleTimeout_(idleTimeout), maxConnections_(maxConnections) {
    loop_.assertInLoopThread();
    if (!listener_ || !timer_) throw systemError(errno, "server socket/timerfd");
    if (idleTimeout.count() <= 0 || maxConnections == 0) throw std::invalid_argument("invalid server limits");
    int one = 1; ::setsockopt(listener_.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(listener_.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) ||
        ::listen(listener_.get(), 128)) throw systemError(errno, "bind/listen");
    socklen_t size = sizeof(address);
    if (::getsockname(listener_.get(), reinterpret_cast<sockaddr*>(&address), &size))
        throw systemError(errno, "getsockname");
    port_ = ntohs(address.sin_port);
    itimerspec spec{}; spec.it_interval.tv_nsec = 100000000; spec.it_value = spec.it_interval;
    if (::timerfd_settime(timer_.get(), 0, &spec, nullptr)) throw systemError(errno, "timerfd_settime");
    acceptChannel_.setReadCallback([this] { acceptReady(); });
    timerChannel_.setReadCallback([this] { timerReady(); });
    acceptChannel_.enableReading(); timerChannel_.enableReading();
}
TcpServer::~TcpServer() { stop(); }
void TcpServer::acceptReady() {
    for (int i = 0; i < 64; ++i) {
        const int fd = ::accept4(listener_.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR) { --i; continue; }
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            // Resource exhaustion must not turn a readable listener into a hot loop.
            acceptPaused_ = true; acceptChannel_.disableAll(); break;
        }
        UniqueFd accepted(fd);
        if (connections_.size() >= maxConnections_) continue;
        auto connection = std::make_shared<TcpConnection>(loop_, std::move(accepted), TcpConnection::Limits{});
        connection->setMessageCallback(messageCallback_);
        connection->setCloseCallback([this](const auto& c) { connections_.erase(c->fd()); });
        connections_.emplace(fd, connection); connection->start();
    }
}
void TcpServer::timerReady() {
    std::uint64_t ticks;
    while (::read(timer_.get(), &ticks, sizeof(ticks)) < 0 && errno == EINTR) {}
    if (acceptPaused_) { acceptPaused_ = false; acceptChannel_.enableReading(); }
    const auto now = std::chrono::steady_clock::now();
    std::vector<TcpConnection::Ptr> expired;
    for (const auto& pair : connections_)
        if (now - pair.second->lastActivity() >= idleTimeout_) expired.push_back(pair.second);
    for (const auto& connection : expired) connection->close();
}
void TcpServer::stop() {
    loop_.assertInLoopThread();
    if (stopped_) return;
    stopped_ = true; acceptChannel_.remove(); timerChannel_.remove();
    while (!connections_.empty()) {
        auto connection = connections_.begin()->second; connection->close();
    }
    listener_.reset(); timer_.reset();
}
} // namespace chunkharbor
