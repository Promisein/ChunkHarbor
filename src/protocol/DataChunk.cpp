#include "chunkharbor/protocol/DataChunk.h"
#include <algorithm>
#include <limits>

namespace chunkharbor::protocol {
namespace {
std::uint64_t readBigEndian(std::string_view bytes, std::size_t start, unsigned count) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < count; ++i)
        value = (value << 8) | static_cast<unsigned char>(bytes[start+i]);
    return value;
}
void writeBigEndian(std::string& bytes, std::size_t start, unsigned count, std::uint64_t value) {
    for (unsigned i = 0; i < count; ++i) {
        bytes[start+count-1-i] = static_cast<char>(value & 255);
        value >>= 8;
    }
}
void validateSize(std::uint64_t offset, std::size_t length) {
    if (length == 0 || length > MaxChunkDataSize)
        throw DataChunkError("invalid DATA_CHUNK data length");
    if (offset > std::numeric_limits<std::uint64_t>::max() - length)
        throw DataChunkError("DATA_CHUNK offset overflow");
}
} // namespace
std::string encodeDataChunk(const DataChunk& chunk) {
    validateSize(chunk.offset, chunk.data.size());
    std::string bytes(DataChunkHeaderSize + chunk.data.size(), '\0');
    for (std::size_t i = 0; i < chunk.sessionId.size(); ++i)
        bytes[i] = static_cast<char>(chunk.sessionId[i]);
    writeBigEndian(bytes, 16, 8, chunk.offset);
    writeBigEndian(bytes, 24, 4, chunk.data.size());
    for (std::size_t i = 0; i < chunk.sha256.size(); ++i)
        bytes[28+i] = static_cast<char>(chunk.sha256[i]);
    bytes.replace(DataChunkHeaderSize, chunk.data.size(), chunk.data);
    return bytes;
}
DataChunk decodeDataChunk(std::string_view payload) {
    if (payload.size() < DataChunkHeaderSize || payload.size() > MaxDataPayload)
        throw DataChunkError("invalid DATA_CHUNK payload size");
    const auto offset = readBigEndian(payload, 16, 8);
    const auto length = readBigEndian(payload, 24, 4);
    // The payload is already bounded. Compare before converting/allocating;
    // subtraction avoids overflow even when the declared uint32 length is huge.
    if (length != payload.size() - DataChunkHeaderSize)
        throw DataChunkError("DATA_CHUNK length mismatch");
    validateSize(offset, static_cast<std::size_t>(length));
    DataChunk chunk;
    for (std::size_t i = 0; i < chunk.sessionId.size(); ++i)
        chunk.sessionId[i] = static_cast<unsigned char>(payload[i]);
    chunk.offset = offset;
    for (std::size_t i = 0; i < chunk.sha256.size(); ++i)
        chunk.sha256[i] = static_cast<unsigned char>(payload[28+i]);
    chunk.data.assign(payload.data() + DataChunkHeaderSize, static_cast<std::size_t>(length));
    return chunk;
}
void validateDataChunkRange(const DataChunk& chunk, std::uint64_t fileSize, std::uint32_t chunkSize) {
    validateSize(chunk.offset, chunk.data.size());
    if (chunkSize == 0 || chunkSize > MaxChunkDataSize)
        throw DataChunkError("invalid session chunk size");
    if (chunk.offset >= fileSize || chunk.offset % chunkSize != 0)
        throw DataChunkError("DATA_CHUNK offset outside aligned file range");
    const auto expected = std::min<std::uint64_t>(chunkSize, fileSize - chunk.offset);
    if (chunk.data.size() != expected)
        throw DataChunkError("DATA_CHUNK must contain a complete block or final remainder");
}
} // namespace chunkharbor::protocol
