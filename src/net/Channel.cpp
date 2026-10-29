#include "chunkharbor/net/Channel.h"
#include "chunkharbor/net/EventLoop.h"

#include <cassert>
#include <sys/epoll.h>

namespace chunkharbor {

Channel::Channel(EventLoop& loop, int fd) noexcept : loop_(loop), fd_(fd) {}
Channel::~Channel() {
    assert(loop_.isInLoopThread());
    if (registered_) remove();
}

void Channel::update() { loop_.updateChannel(*this); }
void Channel::enableReading() { events_ |= EPOLLIN; update(); }
void Channel::enableWriting() { events_ |= EPOLLOUT; update(); }
void Channel::disableAll() { events_ = 0; update(); }
void Channel::remove() { events_ = 0; loop_.removeChannel(*this); }

void Channel::handleEvent(std::uint32_t revents) {
    // Callbacks may change registration. The owner keeps Channel alive until this returns.
    if ((revents & (EPOLLERR | EPOLLHUP)) && errorCallback_) errorCallback_();
    if ((revents & (EPOLLIN | EPOLLRDHUP)) && readCallback_) readCallback_();
    if ((revents & EPOLLOUT) && writeCallback_) writeCallback_();
}

} // namespace chunkharbor
