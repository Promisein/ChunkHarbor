#pragma once
#include "chunkharbor/protocol/DataChunk.h"
#include <vector>

namespace chunkharbor::protocol {
constexpr std::size_t MaxUploadPlanRanges = 512;
class UploadPlanError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
struct ChunkRange { std::uint64_t begin = 0, end = 0; }; // [begin, end)
struct UploadPlan {
    std::array<std::uint8_t, 16> sessionId{};
    std::uint32_t chunkSize = 256 * 1024;
    std::vector<ChunkRange> missingRanges;
};
// JSON body only, Frame type 4. Sorted nonoverlapping ranges; adjacency allowed.
// No file I/O, session authentication, or enumeration of individual blocks.
std::string encodeUploadPlan(const UploadPlan& value);
UploadPlan decodeUploadPlan(std::string_view payload);
// Enforce trusted request geometry in addition to structural checks.
// Empty ranges mean no blocks requested, not proof that a file exists/is committed.
void validateUploadPlanForFile(const UploadPlan& value, std::uint64_t fileSize,
                               std::uint32_t requestedChunkSize);
} // namespace chunkharbor::protocol
