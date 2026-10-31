#pragma once
#include "chunkharbor/protocol/FrameCodec.h"
#include <stdexcept>
#include <vector>

namespace chunkharbor::protocol {
constexpr std::size_t MaxHandshakePayload = 4096;
class MessageError : public std::runtime_error {
public:
    MessageError(std::string code, std::string message)
        : std::runtime_error(std::move(message)), code(std::move(code)) {}
    std::string code;
};
struct Hello {
    std::string clientName = "chunkharbor-cli";
    std::vector<std::uint16_t> versions{1};
    std::vector<std::string> capabilities{"handshake"};
};
struct HelloAck {
    std::uint16_t version = 1;
    std::string serverName = "chunkharbor-server";
    std::vector<std::string> capabilities;
    std::uint64_t maxControlPayload = MaxControlPayload;
    std::uint64_t maxDataPayload = MaxDataPayload;
};
struct HandshakeError { std::string code, message; };
std::string encodeHello(const Hello& value);
Hello decodeHello(std::string_view payload);
std::string encodeHelloAck(const HelloAck& value);
HelloAck decodeHelloAck(std::string_view payload);
std::string encodeHandshakeError(const HandshakeError& value);
HandshakeError decodeHandshakeError(std::string_view payload);
HelloAck negotiateHello(const Hello& offer);
} // namespace chunkharbor::protocol
