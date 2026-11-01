#ifdef NDEBUG
#undef NDEBUG
#endif
#include "chunkharbor/client_core/DeltaPlanner.h"
#include <cassert>
int main() {
    using namespace chunkharbor::client_core;
    BlockDescriptor a{0, 1, {}};
    assert(compareBlock(a, a) == BlockAction::Reuse);
    assert(compareBlock(a, {}) == BlockAction::Upload);
    auto b = a;
    b.length = 2;
    assert(compareBlock(a, b) == BlockAction::Upload);
    b = a;
    ++b.index;
    assert(compareBlock(a, b) == BlockAction::Upload);
    b = a;
    ++b.sha256[0];
    assert(compareBlock(a, b) == BlockAction::Upload);
    RangePageBuilder r;
    unsigned pages = 0, ranges = 0;
    for (unsigned i = 0; i < 65536; i += 2) {
        r.push(i);
        auto p = r.takePage();
        assert(p.size() <= 512);
        if (!p.empty()) {
            ++pages;
            ranges += p.size();
        }
    }
    r.finish();
    auto p = r.takePage();
    if (!p.empty()) {
        ++pages;
        ranges += p.size();
    }
    assert(pages == 64 && ranges == 32768);
    RangePageBuilder contiguous;
    for (unsigned i = 0; i < 65536; ++i) {
        contiguous.push(i);
        assert(contiguous.takePage().empty());
    }
    contiguous.finish();
    auto one = contiguous.takePage();
    assert(one.size() == 1 && one[0].end == 65536);
    PlanTotals t;
    t.add(BlockAction::Reuse, 3);
    t.add(BlockAction::Upload, 2);
    assert(t.missingBlocks == 1 && t.missingBytes == 2 && t.reusedBytes == 3);
    RangePageBuilder blocked;
    for (unsigned i = 0; i < 1026; i += 2)
        blocked.push(i);
    bool caught = false;
    try {
        blocked.finish();
    } catch (...) {
        caught = true;
    }
    assert(caught);
}
