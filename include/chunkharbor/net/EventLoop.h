#pragma once

#include "chunkharbor/common/UniqueFd.h"
#include "chunkharbor/net/EpollPoller.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace chunkharbor {

class Channel;

class EventLoop {
public:
    explicit EventLoop(std::size_t maxPendingTasks = 4096);
    ~EventLoop();
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    void run();
    void quit() noexcept;
    void runInLoop(std::function<void()> task);
    void queueInLoop(std::function<void()> task);
    bool isInLoopThread() const noexcept { return std::this_thread::get_id() == owner_; }
    void assertInLoopThread() const;
    void updateChannel(Channel& channel);
    void removeChannel(Channel& channel);

private:
    void wake() noexcept;
    void drainWakeFd();
    void runPending();

    const std::thread::id owner_;
    const std::size_t maxPendingTasks_;
    EpollPoller poller_;
    UniqueFd wakeFd_;
    std::unique_ptr<Channel> wakeChannel_;
    std::mutex pendingMutex_;
    std::vector<std::function<void()>> pending_;
    std::atomic<bool> quit_{false};
    bool running_ = false;
};

} // namespace chunkharbor
