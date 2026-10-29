#include "chunkharbor/net/EpollPoller.h"
#include "chunkharbor/common/Error.h"
#include "chunkharbor/net/Channel.h"

#include <cerrno>
#include <stdexcept>
#include <sys/epoll.h>

namespace chunkharbor {

EpollPoller::EpollPoller() : epollFd_(::epoll_create1(EPOLL_CLOEXEC)) {
    if (!epollFd_) throw systemError(errno, "epoll_create1");
}

void EpollPoller::update(Channel& channel) {
    if (channel.events() == 0) { remove(channel); return; }
    epoll_event event{};
    event.events = channel.events();
    auto found = tokensByFd_.find(channel.fd());
    if (found != tokensByFd_.end() && entries_.at(found->second).channel != &channel)
        throw std::logic_error("FD already registered by another channel");
    const bool existing = found != tokensByFd_.end();
    const std::uint64_t token = existing ? found->second : nextToken_++;
    event.data.u64 = token;
    if (::epoll_ctl(epollFd_.get(), existing ? EPOLL_CTL_MOD : EPOLL_CTL_ADD,
                    channel.fd(), &event) < 0)
        throw systemError(errno, "epoll_ctl update");
    if (!existing) {
        tokensByFd_.emplace(channel.fd(), token);
        entries_.emplace(token, Entry{channel.fd(), &channel});
    }
    channel.registered_ = true;
}

void EpollPoller::remove(Channel& channel) {
    auto found = tokensByFd_.find(channel.fd());
    if (found == tokensByFd_.end()) return;
    if (entries_.at(found->second).channel != &channel)
        throw std::logic_error("channel does not own FD registration");
    if (::epoll_ctl(epollFd_.get(), EPOLL_CTL_DEL, channel.fd(), nullptr) < 0 && errno != ENOENT)
        throw systemError(errno, "epoll_ctl delete");
    entries_.erase(found->second);
    tokensByFd_.erase(found);
    channel.registered_ = false;
}

std::vector<EpollPoller::ActiveEvent> EpollPoller::poll() {
    epoll_event events[32];
    int count;
    do { count = ::epoll_wait(epollFd_.get(), events, 32, -1); }
    while (count < 0 && errno == EINTR);
    if (count < 0) throw systemError(errno, "epoll_wait");
    std::vector<ActiveEvent> active;
    active.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i)
        active.push_back({events[i].data.u64, events[i].events});
    return active;
}

Channel* EpollPoller::find(std::uint64_t token) const noexcept {
    auto found = entries_.find(token);
    return found == entries_.end() ? nullptr : found->second.channel;
}

} // namespace chunkharbor
