#include "chunkharbor/net/EventLoop.h"
#include "chunkharbor/net/TcpConnection.h"
#include <iostream>
#include <stdexcept>
#include <sys/socket.h>

int main() {
    using namespace chunkharbor;
    try {
        EventLoop loop;
        int pair[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair))
            return 1;
        UniqueFd peer(pair[1]);
        auto connection =
            std::make_shared<TcpConnection>(loop, UniqueFd(pair[0]), TcpConnection::Limits{});
        bool paused = false, replayed = false;
        int calls = 0;
        connection->setMessageCallback([&](const auto &c, Buffer &input) {
            if (++calls == 1) {
                input.consume(1);
                c->pauseReading();
                if (!c->idleTimeoutSuspended())
                    throw std::runtime_error("pending application work must suspend idle expiry");
                c->send("reply"); // output drain must not undo application pause
                loop.queueInLoop([&, c] {
                    paused = c->readPaused();
                    c->resumeReading(); // dispatch already buffered second byte
                    if (c->idleTimeoutSuspended())
                        throw std::runtime_error("completed application work must restore idle expiry");
                    if (!replayed) {
                        c->close();
                        loop.quit();
                    }
                });
            } else {
                replayed = input.readAll() == "b";
                c->close();
                loop.quit();
            }
        });
        connection->start();
        if (::write(peer.get(), "ab", 2) != 2)
            return 2;
        loop.run();
        if (!paused || !replayed || calls != 2)
            throw std::runtime_error("application pause/replay failed");
        connection->pauseReading();
        connection->resumeReading(); // closed is safe
        int drainingPair[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, drainingPair))
            return 4;
        UniqueFd drainingPeer(drainingPair[1]);
        const int sendBuffer = 4096;
        if (::setsockopt(drainingPair[0], SOL_SOCKET, SO_SNDBUF, &sendBuffer, sizeof sendBuffer))
            return 5;
        auto draining = std::make_shared<TcpConnection>(
            loop, UniqueFd(drainingPair[0]), TcpConnection::Limits{});
        draining->start();
        draining->send(std::string(512 * 1024, 'x')); // Force queued output/EAGAIN.
        draining->pauseReading();
        draining->shutdownAfterWrite();
        if (draining->closed() || draining->idleTimeoutSuspended())
            throw std::runtime_error("shutdown/error drain must not suspend idle expiry");
        draining->close();
        std::cout << "application pause survives output drain; buffered input replayed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 3;
    }
}
