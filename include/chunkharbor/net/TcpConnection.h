#pragma once
#include "chunkharbor/common/UniqueFd.h"
#include "chunkharbor/net/Buffer.h"
#include "chunkharbor/net/Channel.h"
#include <chrono>
#include <functional>
#include <memory>

namespace chunkharbor {
class EventLoop;
class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
    struct Limits {
        std::size_t inputLimit = 1024 * 1024 + 32;
        std::size_t outputLimit = 4 * 1024 * 1024;
        std::size_t highWater = 1024 * 1024;
        std::size_t lowWater = 512 * 1024;
    };
    struct Stats { std::size_t shortWrites = 0, wouldBlockWrites = 0, interruptions = 0; };
    using Ptr = std::shared_ptr<TcpConnection>;
    using MessageCallback = std::function<void(const Ptr&, Buffer&)>;
    using CloseCallback = std::function<void(const Ptr&)>;
    TcpConnection(EventLoop& loop, UniqueFd fd, Limits limits);
    void start();
    void send(std::string_view bytes);
    void close();
    void setMessageCallback(MessageCallback cb) { messageCallback_ = std::move(cb); }
    void setCloseCallback(CloseCallback cb) { closeCallback_ = std::move(cb); }
    int fd() const noexcept { return fd_.get(); }
    bool closed() const noexcept { return closed_; }
    bool readPaused() const noexcept { return readPaused_; }
    const Stats& stats() const noexcept { return stats_; }
    std::chrono::steady_clock::time_point lastActivity() const noexcept { return lastActivity_; }
private:
    void readReady();
    void writeReady();
    void refreshInterest();
    EventLoop& loop_;
    UniqueFd fd_;
    Channel channel_;
    Limits limits_;
    Buffer input_, output_;
    bool closed_ = false, started_ = false, readEof_ = false, readPaused_ = false;
    Stats stats_;
    MessageCallback messageCallback_;
    CloseCallback closeCallback_;
    std::chrono::steady_clock::time_point lastActivity_ = std::chrono::steady_clock::now();
};
} // namespace chunkharbor
