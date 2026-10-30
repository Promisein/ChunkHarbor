#include "chunkharbor/net/EventLoop.h"
#include "chunkharbor/net/TcpServer.h"
#include <iostream>
int main() {
    try {
        chunkharbor::EventLoop loop;
        chunkharbor::TcpServer server(loop, 0, std::chrono::seconds(30));
        const auto port = server.port();
        server.stop(); server.stop();
        // Keeping a stopped object alive must not prevent a replacement listener.
        chunkharbor::TcpServer replacement(loop, port, std::chrono::seconds(30));
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
