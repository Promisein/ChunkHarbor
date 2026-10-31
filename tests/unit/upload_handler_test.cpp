#include "UploadHandler.h"
#include "chunkharbor/common/Sha256.h"
#include "chunkharbor/concurrency/DiskExecutor.h"
#include "chunkharbor/net/EventLoop.h"
#include "chunkharbor/protocol/Handshake.h"
#include "chunkharbor/storage/UploadStorage.h"
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <sys/socket.h>
#include <thread>

using namespace chunkharbor;
using namespace chunkharbor::protocol;
int main() {
    try {
        char pattern[] = "/tmp/chunkharbor-handler-XXXXXX";
        const char *root = ::mkdtemp(pattern);
        if (!root)
            return 1;
        struct Cleanup {
            std::filesystem::path p;
            ~Cleanup() {
                std::error_code e;
                std::filesystem::remove_all(p, e);
            }
        } cleanup{root};
        EventLoop loop;
        UploadStorage storage(root);
        DiskExecutor executor(loop, 1, 1);
        int pair[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair))
            return 2;
        ::fcntl(pair[0], F_SETFL, O_NONBLOCK);
        auto connection =
            std::make_shared<TcpConnection>(loop, UniqueFd(pair[0]), TcpConnection::Limits{});
        connection->setMessageCallback(makeUploadHandler(storage, executor, loop));
        connection->start();
        UniqueFd peer(pair[1]);
        timeval timeout{3, 0};
        ::setsockopt(peer.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
        std::exception_ptr error;
        std::thread client([&] {
            try {
                std::string input;
                const auto sendAll = [&](const std::string &wire) {
                    std::size_t sent = 0;
                    while (sent < wire.size()) {
                        auto n = ::send(peer.get(), wire.data() + sent, wire.size() - sent,
                                        MSG_NOSIGNAL);
                        if (n < 0 && errno == EINTR)
                            continue;
                        if (n <= 0)
                            throw std::runtime_error("peer send");
                        sent += n;
                    }
                };
                const auto receive = [&]() {
                    for (;;) {
                        auto result = decode(input);
                        if (result.status == DecodeStatus::Complete) {
                            input.erase(0, result.consumed);
                            return result.frame;
                        }
                        if (result.status == DecodeStatus::Error)
                            throw std::runtime_error("bad response");
                        char buffer[4096];
                        auto n = ::recv(peer.get(), buffer, sizeof buffer, 0);
                        if (n < 0 && errno == EINTR)
                            continue;
                        if (n <= 0)
                            throw std::runtime_error("peer receive");
                        input.append(buffer, n);
                    }
                };
                Hello offer;
                offer.capabilities = {"handshake", "upload"};
                BeginUpload begin{"empty", 0, 0, 4, toHex(sha256(""))};
                sendAll(encode({1, 1, encodeHello(offer)}) +
                        encode({3, 2, encodeBeginUpload(begin)}));
                if (receive().type != 2)
                    throw std::runtime_error("HELLO response");
                auto planFrame = receive();
                if (planFrame.type != 4)
                    throw std::runtime_error("PLAN response");
                auto plan = decodeUploadPlan(planFrame.payload);
                auto commit = encodeCommitUpload({plan.sessionId});
                sendAll(encode({9, 3, commit}) + encode({9, 4, commit}));
                for (std::uint64_t request : {3, 4}) {
                    auto reply = receive();
                    if (reply.type != 10 || reply.requestId != request)
                        throw std::runtime_error("capacity-one continuation returned BUSY");
                    if (decodeCommitResult(reply.payload).size != 0)
                        throw std::runtime_error("commit size");
                }
            } catch (...) {
                error = std::current_exception();
            }
            loop.queueInLoop([&] {
                connection->close();
                loop.quit();
            });
        });
        loop.run();
        client.join();
        executor.stop();
        if (error)
            std::rethrow_exception(error);
        std::cout << "capacity-one buffered upload continuation passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 3;
    }
}
