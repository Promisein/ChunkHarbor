#pragma once
#include <cstdint>
#include <functional>
namespace chunkharbor::client {
struct BatchSummary {
    std::uint64_t succeeded = 0, failed = 0, skipped = 0, cancelled = 0, plannedBytes = 0,
                  acknowledgedBytes = 0;
};
// Serial scheduler deliberately keeps only the current task (queue bound 1 <= 128).
class BackupBatch {
    bool cancelled_ = false;

  public:
    BatchSummary summary;
    void cancel() { cancelled_ = true; }
    bool cancelled() const { return cancelled_; }
    bool step(const std::function<bool()> &next);
};
} // namespace chunkharbor::client
