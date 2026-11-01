#include "chunkharbor/client_core/DeltaPlanner.h"
#include <stdexcept>
namespace chunkharbor::client_core {
BlockAction compareBlock(const BlockDescriptor &a, const std::optional<BlockDescriptor> &b) {
    return b && a.index == b->index && a.length == b->length && a.sha256 == b->sha256
               ? BlockAction::Reuse
               : BlockAction::Upload;
}
void PlanTotals::add(BlockAction a, std::uint32_t n) {
    if (a == BlockAction::Upload) {
        ++missingBlocks;
        missingBytes += n;
    } else {
        ++reusedBlocks;
        reusedBytes += n;
    }
}
void RangePageBuilder::push(std::uint32_t i) {
    if (finished_ || i >= 65536 || (last_ && i <= *last_))
        throw std::invalid_argument("invalid range index");
    if (pending_ && i != pending_->end && ready_.size() == 512)
        throw std::length_error("takePage required before push");
    if (pending_ && i == pending_->end)
        pending_->end = i + 1;
    else {
        if (pending_)
            ready_.push_back(*pending_);
        pending_ = protocol::BlockRange{i, i + 1};
    }
    last_ = i;
}
std::vector<protocol::BlockRange> RangePageBuilder::takePage() {
    // A pending range remains open until a gap or finish, so adjacent ranges never cross pages.
    if (!finished_ && ready_.size() < 512)
        return {};
    auto out = std::move(ready_);
    ready_.clear();
    return out;
}
void RangePageBuilder::finish() {
    if (finished_)
        throw std::logic_error("already finished");
    if (ready_.size() == 512 && pending_)
        throw std::length_error("takePage required before finish");
    if (pending_) {
        ready_.push_back(*pending_);
        pending_.reset();
    }
    finished_ = true;
}
} // namespace chunkharbor::client_core
