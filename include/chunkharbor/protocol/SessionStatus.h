#pragma once
#include "chunkharbor/protocol/UploadPlan.h"
namespace chunkharbor::protocol {
// Type 8: one bit per block, low bit first. 1 means the server needs this block.
struct SessionStatus {
    std::array<std::uint8_t, 16> sessionId{};
    std::uint32_t chunkSize = 0;
    std::uint32_t blockCount = 0;
    std::vector<std::uint8_t> missing;
};
std::string encodeSessionStatus(const SessionStatus &);
SessionStatus decodeSessionStatus(std::string_view);
void validateSessionStatusForFile(const SessionStatus &, std::uint64_t size,
                                  std::uint32_t chunkSize);
} // namespace chunkharbor::protocol
