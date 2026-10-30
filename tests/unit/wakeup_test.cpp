#include "chunkharbor/net/EventLoop.h"
#include <atomic>
#include <cerrno>
#include <sys/epoll.h>
#include <thread>
#include <stdexcept>
#include <unistd.h>
static std::atomic<bool> writeOnce{true}, readOnce{true}, pollOnce{true};
extern "C" ssize_t __real_write(int, const void*, size_t);
extern "C" ssize_t __real_read(int, void*, size_t);
extern "C" int __real_epoll_wait(int, epoll_event*, int, int);
extern "C" ssize_t __wrap_write(int fd, const void* p, size_t n) {
    if (writeOnce.exchange(false)) { errno = EINTR; return -1; }
    return __real_write(fd, p, n);
}
extern "C" ssize_t __wrap_read(int fd, void* p, size_t n) {
    if (readOnce.exchange(false)) { errno = EINTR; return -1; }
    return __real_read(fd, p, n);
}
extern "C" int __wrap_epoll_wait(int fd, epoll_event* p, int n, int timeout) {
    if (pollOnce.exchange(false)) { errno = EINTR; return -1; }
    return __real_epoll_wait(fd, p, n, timeout);
}
int main() {
    chunkharbor::EventLoop loop; std::atomic<int> calls{0};
    std::thread worker([&] { loop.queueInLoop([&] { ++calls; }); }); worker.join();
    std::thread stopper([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        loop.queueInLoop([&] { ++calls; loop.quit(); });
    });
    loop.run(); stopper.join(); loop.quit(); loop.run();
    chunkharbor::EventLoop bounded(1);
    bounded.queueInLoop([] {});
    bool rejected = false;
    try { bounded.queueInLoop([] {}); } catch (const std::length_error&) { rejected = true; }
    bounded.quit(); bounded.run();
    if (!rejected) return 2;
    return calls == 2 && !writeOnce && !readOnce && !pollOnce ? 0 : 1;
}
