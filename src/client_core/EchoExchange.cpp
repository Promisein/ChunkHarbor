#include "chunkharbor/client_core/EchoExchange.h"
namespace chunkharbor::client {
std::string echoRequest(std::string_view message) { return protocol::encode({16, 1, std::string(message)}); }
bool isEchoResponse(const protocol::Frame& response, std::string_view message) {
    return response.type == 17 && response.requestId == 1 && response.payload == message;
}
}
