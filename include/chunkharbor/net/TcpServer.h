#pragma once
#include "chunkharbor/net/TcpConnection.h"
#include <cstdint>
#include <unordered_map>

namespace chunkharbor {
class TcpServer {
public:
    TcpServer(EventLoop& loop, std::uint16_t port, std::chrono::milliseconds idleTimeout,
              std::size_t maxConnections = 512);
    ~TcpServer();
    std::uint16_t port() const noexcept { return port_; }
    void setMessageCallback(TcpConnection::MessageCallback cb) { messageCallback_ = std::move(cb); }
    // Initialization only, on the loop thread before start: install callbacks here.
    // Do not send, close or start the connection from this callback.
    void setConnectionCallback(std::function<void(const TcpConnection::Ptr&)> cb) { connectionCallback_ = std::move(cb); }
    void stop();
private:
    void acceptReady();
    void timerReady();
    EventLoop& loop_;
    UniqueFd listener_, timer_;
    Channel acceptChannel_, timerChannel_;
    std::uint16_t port_ = 0;
    std::chrono::milliseconds idleTimeout_;
    std::size_t maxConnections_;
    bool stopped_ = false, acceptPaused_ = false;
    std::unordered_map<int, TcpConnection::Ptr> connections_;
    TcpConnection::MessageCallback messageCallback_;
    std::function<void(const TcpConnection::Ptr&)> connectionCallback_;
};
} // namespace chunkharbor
