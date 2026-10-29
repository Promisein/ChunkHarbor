#include "chunkharbor/net/EventLoop.h"
#include "chunkharbor/common/Error.h"
#include "chunkharbor/net/Channel.h"

#include <cerrno>
#include <stdexcept>
#include <sys/eventfd.h>
#include <unistd.h>

namespace chunkharbor {

EventLoop::EventLoop()
    : owner_(std::this_thread::get_id()),
      wakeFd_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {
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
    const auto result = ::write(wakeFd_.get(), &one, sizeof(one));
    (void)result; // EAGAIN means an existing wakeup is already pending.
}

void EventLoop::drainWakeFd() {
    std::uint64_t count;
    while (::read(wakeFd_.get(), &count, sizeof(count)) == sizeof(count)) {}
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
    quit_.store(false);
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
}

void EventLoop::quit() noexcept { quit_.store(true); wake(); }

void EventLoop::runInLoop(std::function<void()> task) {
    if (isInLoopThread()) task(); else queueInLoop(std::move(task));
}

void EventLoop::queueInLoop(std::function<void()> task) {
    { std::lock_guard<std::mutex> lock(pendingMutex_); pending_.push_back(std::move(task)); }
    wake();
}

} // namespace chunkharbor
