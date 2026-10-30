#include "chunkharbor/net/TcpConnection.h"
#include "chunkharbor/net/EventLoop.h"
#include <cerrno>
#include <chrono>
#include <cstring>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

static bool interruptSend = true, interruptRecv = true;
extern "C" ssize_t __real_send(int, const void*, size_t, int);
extern "C" ssize_t __real_recv(int, void*, size_t, int);
extern "C" ssize_t __wrap_send(int fd, const void* p, size_t n, int flags) {
    if (interruptSend) { interruptSend = false; errno = EINTR; return -1; }
    return __real_send(fd, p, n > 4096 ? 4096 : n, flags);
}
extern "C" ssize_t __wrap_recv(int fd, void* p, size_t n, int flags) {
    if (interruptRecv) { interruptRecv = false; errno = EINTR; return -1; }
    return __real_recv(fd, p, n, flags);
}
int main() {
    using namespace chunkharbor;
    int fds[2]; if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds)) return 1;
    int small = 4096; ::setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
    EventLoop loop;
    TcpConnection::Limits limits; limits.highWater = 128 * 1024; limits.lowWater = 64 * 1024;
    auto connection = std::make_shared<TcpConnection>(loop, UniqueFd(fds[0]), limits);
    connection->setMessageCallback([](const auto& c, Buffer& b) { auto bytes = b.readAll(); c->send(bytes); });
    connection->setCloseCallback([&](const auto&) { loop.quit(); });
    connection->start();
    const std::string bulk(512 * 1024, 'x'); connection->send(bulk);
    if (!connection->readPaused() || connection->stats().wouldBlockWrites == 0) return 2;
    std::string received;
    bool tailSent = false;
    std::thread peer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        tailSent = ::write(fds[1], "tail", 4) == 4; ::shutdown(fds[1], SHUT_WR);
        char data[8192];
        for (;;) {
            auto n = ::read(fds[1], data, sizeof(data));
            if (n > 0) received.append(data, n);
            else if (n == 0) break;
            else if (errno == EAGAIN || errno == EINTR) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            else break;
        }
        ::close(fds[1]);
    });
    loop.run(); peer.join();
    if (!tailSent || received != bulk + "tail" || connection->stats().shortWrites == 0 ||
        connection->stats().interruptions < 2) return 3;
    int more[2]; if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, more)) return 4;
    UniqueFd other(more[1]);
    auto bounded = std::make_shared<TcpConnection>(loop, UniqueFd(more[0]), limits);
    bounded->start(); bounded->send(std::string(limits.outputLimit + 1, 'z'));
    return bounded->closed() ? 0 : 5;
}
