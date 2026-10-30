#include "EchoClient.h"
#include "chunkharbor/client_core/EchoExchange.h"
#include "chunkharbor/common/UniqueFd.h"
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
namespace {
using Clock = std::chrono::steady_clock;
void waitFor(int fd, short events, Clock::time_point deadline) {
    for (;;) {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (left <= 0) throw std::runtime_error("echo timeout");
        pollfd p{fd, events, 0};
        const int count = ::poll(&p, 1, static_cast<int>(left));
        if (count > 0) return;
        if (count < 0 && errno == EINTR) continue;
        throw std::runtime_error("echo poll timeout/error");
    }
}
}
std::string runEcho(std::uint16_t port, std::string_view message) {
    using namespace chunkharbor;
    const auto request = client::echoRequest(message);
    UniqueFd socket(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!socket) throw std::runtime_error("socket failed");
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    if (::connect(socket.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address))) {
        if (errno != EINPROGRESS && errno != EINTR) throw std::runtime_error("connect failed");
        waitFor(socket.get(), POLLOUT, deadline);
        int error = 0; socklen_t size = sizeof(error);
        if (::getsockopt(socket.get(), SOL_SOCKET, SO_ERROR, &error, &size) || error)
            throw std::runtime_error("connect failed");
    }
    std::size_t sent = 0;
    while (sent < request.size()) {
        const auto n = ::send(socket.get(), request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
        if (n > 0) sent += n;
        else if (n < 0 && errno == EINTR) continue;
        else if (n < 0 && errno == EAGAIN) waitFor(socket.get(), POLLOUT, deadline);
        else throw std::runtime_error("send failed");
    }
    ::shutdown(socket.get(), SHUT_WR);
    std::string bytes;
    for (;;) {
        const auto decoded = protocol::decode(bytes);
        if (decoded.status == protocol::DecodeStatus::Error) throw std::runtime_error(decoded.error);
        if (decoded.status == protocol::DecodeStatus::Complete) {
            if (!client::isEchoResponse(decoded.frame, message)) throw std::runtime_error("echo mismatch");
            return decoded.frame.payload;
        }
        char buffer[8192];
        const auto n = ::recv(socket.get(), buffer, sizeof(buffer), 0);
        if (n > 0) bytes.append(buffer, n);
        else if (n < 0 && errno == EINTR) continue;
        else if (n < 0 && errno == EAGAIN) waitFor(socket.get(), POLLIN, deadline);
        else throw std::runtime_error("server closed before complete response");
    }
}
