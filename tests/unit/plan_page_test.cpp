#ifdef NDEBUG
#undef NDEBUG
#endif
#include "chunkharbor/protocol/PlanPage.h"
#include <cassert>
#include <functional>
using namespace chunkharbor::protocol;
void rejects(std::function<void()> f) {
    bool caught = false;
    try {
        f();
    } catch (...) {
        caught = true;
    }
    assert(caught);
}
int main() {
    PlanDone change;
    auto encodedChange = encodePlanDone(change);
    assert(encodedChange.find("\"change_kind\":\"Modified\"") != std::string::npos);
    assert(decodePlanDone(encodedChange).changeKind == "Modified");
    for (auto kind : {"Added", "Modified", "Unchanged"}) {
        change.changeKind = kind;
        assert(decodePlanDone(encodePlanDone(change)).changeKind == kind);
    }
    change.changeKind = "invalid";
    rejects([&] { encodePlanDone(change); });
    rejects([] {
        decodePlanDone("{\"base_version\":null,\"block_count\":0,\"missing_blocks\":0,\"missing_"
                       "bytes\":0,\"page_count\":0,\"reused_blocks\":0,\"reused_bytes\":0,"
                       "\"session_id\":\"00000000000000000000000000000000\"}");
    });
    PlanPage p;
    p.missingRanges = {{0, 1}, {2, 3}};
    auto s = encodePlanPage(p);
    assert(decodePlanPage(s).missingRanges.size() == 2);
    assert(s == "{\"base_version\":null,\"missing_ranges\":[[0,1],[2,3]],\"page_index\":0,"
                "\"session_id\":\"00000000000000000000000000000000\"}");
    PlanValidator v({}, {}, 3, 1);
    v.append(p);
    v.finish({{}, {}, 1, 3, 2, 2, 1, 1});
    rejects([&] { v.finish({}); });
    p.missingRanges = {{0, 1}, {1, 2}};
    rejects([&] { encodePlanPage(p); });
    p.missingRanges.clear();
    rejects([&] { encodePlanPage(p); });
    for (unsigned i = 0; i < 512; ++i)
        p.missingRanges.push_back({i * 2, i * 2 + 1});
    assert(decodePlanPage(encodePlanPage(p)).missingRanges.size() == 512);
    p.missingRanges.push_back({1024, 1025});
    rejects([&] { encodePlanPage(p); });
    PlanValidator cross({}, {}, 10, 1);
    p.missingRanges = {{0, 2}};
    cross.append(p);
    p.pageIndex = 1;
    p.missingRanges = {{2, 3}};
    rejects([&] { cross.append(p); });
    p.missingRanges = {{3, 4}};
    cross.append(p);
    rejects([&] { cross.finish({{}, {}, 2, 10, 3, 99, 7, 7}); });
    PlanValidator empty({}, {}, 0, 1);
    empty.finish({});
    PlanValidator tail({}, {}, 5, 3);
    p.pageIndex = 0;
    p.missingRanges = {{1, 2}};
    tail.append(p);
    tail.finish({{}, {}, 1, 2, 1, 2, 1, 3});
    rejects([] { decodePlanPage("{\"session_id\":0}"); });
    rejects([] { PlanValidator({}, {}, 67108865, 1); });
}
