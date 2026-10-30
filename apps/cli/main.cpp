#include "EchoClient.h"
#include <charconv>
#include <iostream>
#include <stdexcept>
#include <string_view>

int main(int argc, char* argv[]) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--help") {
            std::cout << "Usage: chunkharbor-cli echo [--host 127.0.0.1] [--port 8787] [--message TEXT]\n"
                      << "M1 framed Echo only; file backup is not implemented.\n"; return 0;
        }
        if (argc < 2 || std::string_view(argv[1]) != "echo") throw std::invalid_argument("use echo or --help");
        unsigned port = 8787; std::string message = "hello ChunkHarbor";
        for (int i = 2; i < argc; i += 2) {
            if (i + 1 >= argc) throw std::invalid_argument("missing option value");
            const std::string_view option(argv[i]), value(argv[i+1]);
            if (option == "--message") message = value;
            else if (option == "--host") {
                if (value != "127.0.0.1") throw std::invalid_argument("M1 CLI uses 127.0.0.1 only");
            } else if (option == "--port") {
                auto parsed = std::from_chars(value.data(), value.data() + value.size(), port);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || port == 0 || port > 65535)
                    throw std::invalid_argument("invalid port");
            } else throw std::invalid_argument("unknown option");
        }
        std::cout << "ECHO OK: " << runEcho(static_cast<std::uint16_t>(port), message) << '\n';
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 2; }
}
