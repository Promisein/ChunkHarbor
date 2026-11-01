#pragma once
#include "chunkharbor/protocol/DeltaControl.h"
#include <vector>
namespace chunkharbor::protocol {
struct BlockRange {
    std::uint32_t begin = 0, end = 0;
};
struct PlanPage {
    SessionId sessionId{};
    std::optional<VersionId> baseVersion;
    std::uint32_t pageIndex = 0;
    std::vector<BlockRange> missingRanges;
};
struct PlanDone {
    SessionId sessionId{};
    std::optional<VersionId> baseVersion;
    std::uint32_t pageCount = 0, blockCount = 0;
    std::uint64_t missingBlocks = 0, missingBytes = 0, reusedBlocks = 0, reusedBytes = 0;
    std::string changeKind = "Modified";
};
std::string encodePlanPage(const PlanPage &);
PlanPage decodePlanPage(std::string_view);
std::string encodePlanDone(const PlanDone &);
PlanDone decodePlanDone(std::string_view);
// Streaming validator retains only the previous boundary and counters.
class PlanValidator {
  public:
    PlanValidator(SessionId session, std::optional<VersionId> base, std::uint64_t size,
                  std::uint32_t chunkSize);
    void append(const PlanPage &);
    void finish(const PlanDone &);

  private:
    SessionId session_;
    std::optional<VersionId> base_;
    std::uint64_t size_, missingBlocks_ = 0, missingBytes_ = 0;
    std::uint32_t chunkSize_, blocks_, pages_ = 0, lastEnd_ = 0;
    bool finished_ = false;
};
} // namespace chunkharbor::protocol
