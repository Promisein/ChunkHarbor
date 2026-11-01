#pragma once
#include "chunkharbor/protocol/PlanPage.h"
namespace chunkharbor::client_core {
struct BlockDescriptor {
    std::uint32_t index = 0, length = 0;
    std::array<std::uint8_t, 32> sha256{};
};
enum class BlockAction { Reuse, Upload };
BlockAction compareBlock(const BlockDescriptor &, const std::optional<BlockDescriptor> &);
struct PlanTotals {
    std::uint64_t missingBlocks = 0, missingBytes = 0, reusedBlocks = 0, reusedBytes = 0;
    void add(BlockAction, std::uint32_t length);
};
class RangePageBuilder {
  public:
    void push(std::uint32_t index);
    std::vector<protocol::BlockRange> takePage();
    void finish();

  private:
    std::vector<protocol::BlockRange> ready_;
    std::optional<protocol::BlockRange> pending_;
    std::optional<std::uint32_t> last_;
    bool finished_ = false;
};
} // namespace chunkharbor::client_core
