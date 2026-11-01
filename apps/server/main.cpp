#include "EchoHandler.h"
#include "HandshakeHandler.h"
#include "UploadHandler.h"
#include "chunkharbor/common/Error.h"
#include "chunkharbor/concurrency/DiskExecutor.h"
#include "chunkharbor/net/EventLoop.h"
#include "chunkharbor/net/TcpServer.h"
#include "chunkharbor/storage/DeltaStorage.h"
#include "chunkharbor/storage/UploadStorage.h"
#include <charconv>
#include <csignal>
#include <iostream>
#include <pthread.h>
#include <stdexcept>
#include <string_view>
#include <sys/signalfd.h>
#include <unistd.h>

unsigned number(std::string_view text, unsigned max) {
    unsigned value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value > max)
        throw std::invalid_argument("invalid numeric option");
    return value;
}
int main(int argc, char *argv[]) {
    try {
        bool echo = false, handshake = false, upload = false, explicitMax = false;
        std::string root;
        unsigned port = 8787, idleMs = 30000, maxConnections = 512;
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg(argv[i]);
            if (arg == "--help") {
                std::cout << "Usage: chunkharbor-server (--echo | --handshake | --upload --root "
                             "PRIVATE_ROOT) [--port 8787] [--idle-timeout-ms 30000] "
                             "[--max-connections 512]\n"
                          << "Loopback only; upload requires an existing private absolute root. No "
                             "authentication.\n";
                return 0;
            }
            if (arg == "--echo") {
                echo = true;
                continue;
            }
            if (arg == "--handshake") {
                handshake = true;
                continue;
            }
            if (arg == "--upload") {
                upload = true;
                continue;
            }
            if (i + 1 >= argc)
                throw std::invalid_argument("missing option value");
            const std::string_view value(argv[++i]);
            if (arg == "--port")
                port = number(value, 65535);
            else if (arg == "--idle-timeout-ms")
                idleMs = number(value, 3600000);
            else if (arg == "--max-connections") {
                maxConnections = number(value, 10000);
                explicitMax = true;
            } else if (arg == "--root")
                root = std::string(value);
            else
                throw std::invalid_argument("unknown option");
        }
        if (static_cast<int>(echo) + static_cast<int>(handshake) + static_cast<int>(upload) != 1)
            throw std::invalid_argument("select exactly one server mode");
        if (upload && root.empty())
            throw std::invalid_argument("--upload requires --root");
        if (!upload && !root.empty())
            throw std::invalid_argument("--root requires --upload");
        if (upload && !explicitMax)
            maxConnections = 32;
        if (!maxConnections || (upload && maxConnections > 64))
            throw std::invalid_argument("invalid connection limit");
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGINT);
        sigaddset(&mask, SIGTERM);
        const auto error = ::pthread_sigmask(SIG_BLOCK, &mask, nullptr);
        if (error)
            throw chunkharbor::systemError(error, "pthread_sigmask");
        chunkharbor::EventLoop loop;
        std::unique_ptr<chunkharbor::UploadStorage> storage;
        std::unique_ptr<chunkharbor::DeltaStorage> delta;
        std::unique_ptr<chunkharbor::DiskExecutor> executor;
        if (upload) {
            storage = std::make_unique<chunkharbor::UploadStorage>(root);
            delta = std::make_unique<chunkharbor::DeltaStorage>(*storage);
            executor = std::make_unique<chunkharbor::DiskExecutor>(loop, 2, 64);
        }
        chunkharbor::TcpServer server(loop, port, std::chrono::milliseconds(idleMs),
                                      maxConnections);
        if (echo)
            server.setMessageCallback(echoFrames);
        else if (upload)
            server.setConnectionCallback([&](const auto &connection) {
                connection->setMessageCallback(
                    makeUploadHandler(*storage, *executor, loop, delta.get()));
            });
        else
            server.setConnectionCallback([](const auto &connection) {
                connection->setMessageCallback(makeHandshakeHandler());
            });
        chunkharbor::UniqueFd signalFd(::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC));
        if (!signalFd)
            throw chunkharbor::systemError(errno, "signalfd");
        chunkharbor::Channel signals(loop, signalFd.get());
        signals.setReadCallback([&] {
            signalfd_siginfo info{};
            ssize_t n;
            do {
                n = ::read(signalFd.get(), &info, sizeof(info));
            } while (n < 0 && errno == EINTR);
            if (n == sizeof(info))
                loop.quit();
        });
        signals.enableReading();
        std::cout << "LISTENING 127.0.0.1:" << server.port() << std::endl;
        loop.run();
        server.stop();
        if (executor)
            executor->stop();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
