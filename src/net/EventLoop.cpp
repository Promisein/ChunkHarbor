#include "chunkharbor/net/EventLoop.h"
#include "chunkharbor/common/Error.h"
#include "chunkharbor/net/Channel.h"

#include <cerrno>
#include <stdexcept>
#include <sys/eventfd.h>
#include <unistd.h>

namespace chunkharbor {

EventLoop::EventLoop(std::size_t maxPendingTasks)
    : owner_(std::this_thread::get_id()), maxPendingTasks_(maxPendingTasks),
      wakeFd_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {
    if (maxPendingTasks == 0) throw std::invalid_argument("pending task limit is zero");
    if (!wakeFd_) throw systemError(errno, "eventfd");
    wakeChannel_ = std::make_unique<Channel>(*this, wakeFd_.get());
    wakeChannel_->setReadCallback([this] { drainWakeFd(); });
    wakeChannel_->enableReading();
}

EventLoop::~EventLoop() {
    if (running_ || !isInLoopThread()) std::terminate();
    wakeChannel_.reset();
}

void EventLoop::assertInLoopThread() const {
    if (!isInLoopThread()) throw std::logic_error("EventLoop used from wrong thread");
}

void EventLoop::updateChannel(Channel& channel) { assertInLoopThread(); poller_.update(channel); }
void EventLoop::removeChannel(Channel& channel) { assertInLoopThread(); poller_.remove(channel); }

void EventLoop::wake() noexcept {
    const std::uint64_t one = 1;
    ssize_t result;
    do { result = ::write(wakeFd_.get(), &one, sizeof(one)); } while (result < 0 && errno == EINTR);
    (void)result; // EAGAIN means an existing wakeup is already pending.
}

void EventLoop::drainWakeFd() {
    std::uint64_t count;
    for (;;) {
        const auto result = ::read(wakeFd_.get(), &count, sizeof(count));
        if (result == sizeof(count) || (result < 0 && errno == EINTR)) continue;
        if (result < 0 && errno == EAGAIN) break;
        throw systemError(errno, "eventfd read");
    }
}

void EventLoop::runPending() {
    std::vector<std::function<void()>> tasks;
    { std::lock_guard<std::mutex> lock(pendingMutex_); tasks.swap(pending_); }
    for (auto& task : tasks) task();
}

void EventLoop::run() {
    assertInLoopThread();
    if (running_) throw std::logic_error("EventLoop already running");
    running_ = true;
    try {
        runPending();
        while (!quit_.load()) {
            for (const auto& event : poller_.poll()) {
                if (auto* channel = poller_.find(event.token)) channel->handleEvent(event.events);
            }
            runPending();
        }
    } catch (...) { running_ = false; throw; }
    running_ = false;
    quit_.store(false);
}

void EventLoop::quit() noexcept { quit_.store(true); wake(); }

void EventLoop::runInLoop(std::function<void()> task) {
    if (isInLoopThread()) task(); else queueInLoop(std::move(task));
}

void EventLoop::queueInLoop(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        if (pending_.size() >= maxPendingTasks_) throw std::length_error("EventLoop pending queue full");
        pending_.push_back(std::move(task));
    }
    wake();
}

} // namespace chunkharbor
