#include "chunkharbor/protocol/FrameCodec.h"
#include <stdexcept>

namespace chunkharbor::protocol {
namespace {
std::uint64_t get(std::string_view bytes, std::size_t offset, unsigned count) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < count; ++i)
        value = (value << 8) | static_cast<unsigned char>(bytes[offset + i]);
    return value;
}
void put(std::string &bytes, std::size_t offset, unsigned count, std::uint64_t value) {
    for (unsigned i = 0; i < count; ++i) {
        bytes[offset + count - 1 - i] = static_cast<char>(value & 255);
        value >>= 8;
    }
}
std::size_t limit(std::uint16_t type) {
    return type == 5 || type == 14 ? MaxDataPayload : MaxControlPayload;
}
} // namespace
std::string encode(const Frame &frame) {
    if (frame.type < 1 || frame.type > 31)
        throw std::invalid_argument("unknown frame type");
    if (frame.payload.size() > limit(frame.type))
        throw std::length_error("frame too large");
    std::string bytes(HeaderSize + frame.payload.size(), '\0');
    put(bytes, 0, 4, 0x43484b48);
    put(bytes, 4, 2, 1);
    put(bytes, 6, 2, frame.type);
    put(bytes, 12, 8, frame.requestId);
    put(bytes, 20, 8, frame.payload.size());
    bytes.replace(HeaderSize, frame.payload.size(), frame.payload);
    return bytes;
}
DecodeResult decode(std::string_view bytes) {
    DecodeResult result;
    if (bytes.size() < HeaderSize)
        return result;
    const auto type = static_cast<std::uint16_t>(get(bytes, 6, 2));
    const auto length = get(bytes, 20, 8);
    if (get(bytes, 0, 4) != 0x43484b48 || get(bytes, 4, 2) != 1 || type < 1 || type > 31 ||
        get(bytes, 8, 4) || get(bytes, 28, 4) || length > limit(type)) {
        result.status = DecodeStatus::Error;
        result.error = "invalid frame header";
        return result;
    }
    // length is bounded before conversion/addition or allocation, including on 32-bit hosts.
    const auto total = HeaderSize + static_cast<std::size_t>(length);
    if (bytes.size() < total)
        return result;
    result.status = DecodeStatus::Complete;
    result.consumed = total;
    result.frame = {type, get(bytes, 12, 8), std::string(bytes.substr(HeaderSize, length))};
    return result;
}
} // namespace chunkharbor::protocol
