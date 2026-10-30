#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace chunkharbor::protocol {
constexpr std::size_t HeaderSize = 32;
constexpr std::size_t MaxControlPayload = 64 * 1024;
constexpr std::size_t MaxDataPayload = 1024 * 1024;
struct Frame {
    std::uint16_t type = 16;
    std::uint64_t requestId = 0;
    std::string payload;
};
enum class DecodeStatus { NeedMore, Complete, Error };
struct DecodeResult {
    DecodeStatus status = DecodeStatus::NeedMore;
    std::size_t consumed = 0;
    Frame frame;
    std::string error;
};
// Stateless incremental parser: the caller retains incomplete bytes. One frame per call.
DecodeResult decode(std::string_view bytes);
std::string encode(const Frame& frame);
} // namespace chunkharbor::protocol
