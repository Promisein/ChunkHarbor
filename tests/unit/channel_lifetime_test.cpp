#include "chunkharbor/net/Channel.h"
#include "chunkharbor/net/EventLoop.h"
#include "chunkharbor/common/UniqueFd.h"
#include <memory>
#include <stdexcept>
#include <sys/epoll.h>
#include <unistd.h>

using namespace chunkharbor;
struct Owner { Channel channel; explicit Owner(EventLoop& loop, int fd) : channel(loop, fd) {} };
int main() {
    int fds[2]; if (::pipe(fds)) return 1;
    UniqueFd in(fds[0]), out(fds[1]); EventLoop loop;
    auto owner = std::make_shared<Owner>(loop, in.get());
    std::weak_ptr<Owner> weak = owner;
    owner->channel.tie(owner);
    int calls = 0;
    owner->channel.setReadCallback([&] {
        owner.reset();
        if (weak.expired()) throw std::runtime_error("owner died during callback");
        ++calls;
    });
    owner->channel.enableReading();
    owner->channel.handleEvent(EPOLLIN);
    if (calls != 1 || !weak.expired()) return 2;
    Channel channel(loop, in.get());
    auto guard = std::make_shared<int>(0); channel.tie(guard);
    channel.setReadCallback([&] { ++calls; channel.remove(); });
    channel.setWriteCallback([&] { ++calls; });
    channel.setInterest(true, true); guard.reset();
    channel.handleEvent(EPOLLIN | EPOLLOUT);
    if (calls != 1) return 3;
    guard = std::make_shared<int>(0); channel.tie(guard);
    channel.handleEvent(EPOLLIN | EPOLLOUT);
    return calls == 2 ? 0 : 4;
}
