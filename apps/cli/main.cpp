#include "DeltaClient.h"
#include "EchoClient.h"
#include "UploadClient.h"
#include "chunkharbor/protocol/UploadControl.h"
#include <charconv>
#include <iostream>
#include <stdexcept>
#include <string_view>

int main(int argc, char *argv[]) {
    try {
        if (argc > 1 && (std::string(argv[1]) == "backup" || std::string(argv[1]) == "preview"))
            return runDeltaCommand(argc, argv);
        if (argc == 2 && std::string_view(argv[1]) == "--help") {
            std::cout << "Delta: (backup|preview) --file SOURCE --path LOGICAL --state STATE "
                         "[--chunk-size N] [--port N]\nDirectory: (backup|preview) --directory "
                         "ROOT --prefix LOGICAL --state-dir DIR [--exclude COMPONENT]\nPreview "
                         "conditions: --base-version ID|absent --source-sha256 HASH\n";
            std::cout << "Usage: chunkharbor-cli (echo [--message TEXT] | hello [--client-name "
                         "NAME]) [--host 127.0.0.1] [--port 8787]\n"
                      << "Upload: upload --file SOURCE --path LOGICAL [--host 127.0.0.1] [--port "
                         "8787] [--chunk-size 262144]. Trusted local/SSH only.\n";
            return 0;
        }
        if (argc < 2)
            throw std::invalid_argument("use echo, hello or --help");
        if (std::string_view(argv[1]) == "upload") {
            std::string source, path, host = "127.0.0.1";
            unsigned port = 8787, chunk = 262144;
            for (int i = 2; i < argc; i += 2) {
                if (i + 1 >= argc)
                    throw std::invalid_argument("missing option value");
                std::string_view key(argv[i]), value(argv[i + 1]);
                if (key == "--file")
                    source = value;
                else if (key == "--path")
                    path = value;
                else if (key == "--host")
                    host = value;
                else if (key == "--port" || key == "--chunk-size") {
                    unsigned number = 0;
                    auto parsed =
                        std::from_chars(value.data(), value.data() + value.size(), number);
                    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                        number == 0)
                        throw std::invalid_argument("invalid numeric option");
                    if (key == "--port") {
                        if (number > 65535)
                            throw std::invalid_argument("invalid port");
                        port = number;
                    } else
                        chunk = number;
                } else
                    throw std::invalid_argument("unknown upload option");
            }
            if (source.empty() || path.empty())
                throw std::invalid_argument("upload requires --file and --path");
            runUpload(source, path, host, static_cast<std::uint16_t>(port), chunk);
            return 0;
        }
        const bool hello = std::string_view(argv[1]) == "hello";
        if (!hello && std::string_view(argv[1]) != "echo")
            throw std::invalid_argument("unknown command");
        unsigned port = 8787;
        std::string message = "hello ChunkHarbor", clientName = "chunkharbor-cli";
        for (int i = 2; i < argc; i += 2) {
            if (i + 1 >= argc)
                throw std::invalid_argument("missing option value");
            const std::string_view option(argv[i]), value(argv[i + 1]);
            if (option == "--message" && !hello)
                message = value;
            else if (option == "--client-name" && hello)
                clientName = value;
            else if (option == "--host") {
                if (value != "127.0.0.1")
                    throw std::invalid_argument("M1 CLI uses 127.0.0.1 only");
            } else if (option == "--port") {
                auto parsed = std::from_chars(value.data(), value.data() + value.size(), port);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                    port == 0 || port > 65535)
                    throw std::invalid_argument("invalid port");
            } else
                throw std::invalid_argument("unknown option");
        }
        if (hello) {
            const auto ack = runHandshake(static_cast<std::uint16_t>(port), clientName);
            std::cout << "HELLO OK: version=" << ack.version << " server=" << ack.serverName
                      << " capabilities=";
            for (const auto &capability : ack.capabilities)
                std::cout << capability << ' ';
            std::cout << "control_limit=" << ack.maxControlPayload
                      << " data_limit=" << ack.maxDataPayload << '\n';
        } else
            std::cout << "ECHO OK: " << runEcho(static_cast<std::uint16_t>(port), message) << '\n';
    } catch (const chunkharbor::protocol::UploadFailure &error) {
        std::cerr << error.code << ": " << error.what() << '\n';
        return 2;
    } catch (const chunkharbor::protocol::MessageError &error) {
        std::cerr << error.code << ": " << error.what() << '\n';
        return 2;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
