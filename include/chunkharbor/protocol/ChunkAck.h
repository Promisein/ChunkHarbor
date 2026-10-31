#pragma once
#include "chunkharbor/protocol/DataChunk.h"

namespace chunkharbor::protocol {
class ChunkAckError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
// This message claims durable acceptance of exactly one block, not file commit.
// Construct/encode only AFTER verification, durable write and metadata commit.
// The codec cannot inspect disk state or certify that these steps occurred.
struct ChunkAck {
    std::array<std::uint8_t, 16> sessionId{};
    std::uint64_t offset = 0;
    std::uint32_t dataLength = 0;
    std::array<std::uint8_t, 32> sha256{};
};
// Body only, Frame type 6. JSON always contains boolean durable:true.
// Request correlation, session authority and persistence are caller obligations.
std::string encodeChunkAck(const ChunkAck& value);
ChunkAck decodeChunkAck(std::string_view payload);
// Match the ACK identity/offset/length/declared digest to the pending DATA_CHUNK.
// Does not hash data, check request_id, or prove server-side persistence.
void validateChunkAckForChunk(const ChunkAck& ack, const DataChunk& pending);
} // namespace chunkharbor::protocol
