#ifdef NDEBUG
#undef NDEBUG
#endif
#include "chunkharbor/protocol/DeltaControl.h"
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
    BeginDelta b;
    b.metadata = {"a", 1, INT64_MIN, 1, std::string(64, '0')};
    assert(decodeBeginDelta(encodeBeginDelta(b)).metadata.mtimeNs == INT64_MIN);
    b.base.kind = BaseSelector::Kind::Exact;
    b.base.versionId[0] = 255;
    assert(decodeBeginDelta(encodeBeginDelta(b)).base.versionId[0] == 255);
    b.base.kind = BaseSelector::Kind::Absent;
    assert(decodeBeginDelta(encodeBeginDelta(b)).base.kind == BaseSelector::Kind::Absent);
    DeltaStatus st;
    assert(decodeDeltaStatus(encodeDeltaStatus(st)).phase == DeltaPhase::Collecting);
    for (auto phase :
         {DeltaPhase::Collecting, DeltaPhase::PlanReady, DeltaPhase::Transferring,
          DeltaPhase::Verifying, DeltaPhase::Committing, DeltaPhase::Failed, DeltaPhase::Expired}) {
        st.phase = phase;
        assert(decodeDeltaStatus(encodeDeltaStatus(st)).phase == phase);
    }
    st.missingBitmap = "01";
    rejects([&] { encodeDeltaStatus(st); });
    st.phase = DeltaPhase::Transferring;
    assert(decodeDeltaStatus(encodeDeltaStatus(st)).missingBitmap == "01");
    st.mode = DeltaMode::Preview;
    rejects([&] { encodeDeltaStatus(st); });
    DeltaResult result;
    result.path = "a";
    result.sha256 = std::string(64, '0');
    st = {};
    st.phase = DeltaPhase::Committed;
    st.result = result;
    assert(decodeDeltaStatus(encodeDeltaStatus(st)).result->committed);
    assert(decodeDeltaReady(encodeDeltaReady({{}, {}, 65536})).blockCount == 65536);
    assert(decodeManifestDone(encodeManifestDone({{}, 128, 65536})).pageCount == 128);
    rejects([] { encodeManifestDone({{}, 1, 0}); });
    assert(decodeManifestAck(encodeManifestAck({{}, 128, DeltaPhase::PlanReady})).nextPageIndex ==
           128);
    assert(decodeGetPlanPage(encodeGetPlanPage({{}, 64})).pageIndex == 64);
    rejects([] { encodeGetPlanPage({{}, 65}); });
    assert(encodeQueryDelta({}) == "{\"session_id\":\"00000000000000000000000000000000\"}");
    assert(decodeCommitDelta(encodeCommitDelta({})).sessionId == SessionId{});
    for (auto s :
         {"{\"session_id\":\"00000000000000000000000000000000\",\"session_id\":"
          "\"00000000000000000000000000000000\"}",
          "{\"session_id\":1}", "{\"session_id\":null}", "{\"session_id\":\"\\u0000\"}",
          "{\"session_id\":[[[[[0]]]]]}", "{\"session_id\":\"0000000000000000000000000000000A\"}",
          "{\"session_id\":\"00000000000000000000000000000000\",\"x\":0}"})
        rejects([&] { decodeQueryDelta(s); });
    rejects([] {
        decodeGetPlanPage(
            "{\"session_id\":\"00000000000000000000000000000000\",\"page_index\":1.0}");
    });
    rejects([] { decodeQueryDelta(std::string(65537, ' ')); });
}
