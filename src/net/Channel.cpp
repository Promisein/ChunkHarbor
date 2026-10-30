#include "chunkharbor/net/Channel.h"
#include "chunkharbor/net/EventLoop.h"

#include <cassert>
#include <exception>
#include <sys/epoll.h>

namespace chunkharbor {

Channel::Channel(EventLoop& loop, int fd) noexcept : loop_(loop), fd_(fd) {}
Channel::~Channel() {
    if (!loop_.isInLoopThread()) std::terminate();
    if (registered_) remove();
}

void Channel::update() { loop_.updateChannel(*this); }
void Channel::setInterest(bool read, bool write) {
    loop_.assertInLoopThread();
    const std::uint32_t next = (read ? EPOLLIN | EPOLLRDHUP : 0u) | (write ? EPOLLOUT : 0u);
    if (next == events_) return;
    const auto previous = events_;
    events_ = next;
    try { update(); } catch (...) { events_ = previous; throw; }
}
void Channel::enableReading() { setInterest(true, events_ & EPOLLOUT); }
void Channel::enableWriting() { setInterest(events_ & EPOLLIN, true); }
void Channel::disableAll() { setInterest(false, false); }
void Channel::remove() { loop_.assertInLoopThread(); events_ = 0; loop_.removeChannel(*this); }

void Channel::handleEvent(std::uint32_t revents) {
    loop_.assertInLoopThread();
    const auto guard = tied_ ? owner_.lock() : std::shared_ptr<void>{};
    if (tied_ && !guard) return;
    if (!registered_) return;
    if ((revents & EPOLLERR) && errorCallback_) errorCallback_();
    if (!registered_) return;
    if ((revents & (EPOLLIN | EPOLLRDHUP | EPOLLHUP)) && (events_ & EPOLLIN) && readCallback_)
        readCallback_();
    if (!registered_) return;
    if ((revents & EPOLLOUT) && (events_ & EPOLLOUT) && writeCallback_) writeCallback_();
}

} // namespace chunkharbor
