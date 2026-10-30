#pragma once
#include "chunkharbor/protocol/FrameCodec.h"
namespace chunkharbor::client {
std::string echoRequest(std::string_view message);
bool isEchoResponse(const protocol::Frame& response, std::string_view message);
}
