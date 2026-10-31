#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include "chunkharbor/protocol/Handshake.h"
std::string runEcho(std::uint16_t port, std::string_view message);
chunkharbor::protocol::HelloAck runHandshake(std::uint16_t port, std::string_view clientName);
