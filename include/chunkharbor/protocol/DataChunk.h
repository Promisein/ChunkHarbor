#pragma once
#include "chunkharbor/protocol/FrameCodec.h"
#include <array>
#include <stdexcept>

namespace chunkharbor::protocol {
constexpr std::size_t DataChunkHeaderSize = 60;
constexpr std::size_t MaxChunkDataSize = MaxDataPayload - DataChunkHeaderSize;

class DataChunkError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
struct DataChunk {
    std::array<std::uint8_t, 16> sessionId{};
    std::uint64_t offset = 0;
    std::array<std::uint8_t, 32> sha256{};
    std::string data;
};
// Body only: caller selects Frame type 5 and checks request/session state.
// Nonempty data, exact length, payload limit and offset overflow are checked.
// Decoded data is owned, independent of the input lifetime. No hashing or I/O.
std::string encodeDataChunk(const DataChunk& chunk);
DataChunk decodeDataChunk(std::string_view payload);
// Given trusted session geometry, require a complete aligned block (short final
// block allowed). Empty files have no DATA_CHUNK. Does not authenticate sessionId.
void validateDataChunkRange(const DataChunk& chunk, std::uint64_t fileSize,
                            std::uint32_t chunkSize);
} // namespace chunkharbor::protocol
