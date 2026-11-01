#include "chunkharbor/protocol/PlanPage.h"
#include "DeltaJson.h"
namespace chunkharbor::protocol {
using namespace delta_json;
std::string encodePlanPage(const PlanPage &v) {
    if (v.pageIndex >= 64 || v.missingRanges.empty() || v.missingRanges.size() > 512)
        bad();
    J a = J::array();
    std::uint32_t end = 0;
    bool first = true;
    for (auto r : v.missingRanges) {
        if (r.begin >= r.end || r.end > 65536 || (!first && r.begin <= end))
            bad();
        a.push_back(J::array({r.begin, r.end}));
        end = r.end;
        first = false;
    }
    return dump(J{{"session_id", hex(v.sessionId)},
                  {"base_version", base(v.baseVersion)},
                  {"page_index", v.pageIndex},
                  {"missing_ranges", a}});
}
PlanPage decodePlanPage(std::string_view s) {
    auto j = parse(s);
    fields(j, {"session_id", "base_version", "page_index", "missing_ranges"});
    PlanPage v;
    v.sessionId = id<16>(j.at("session_id"));
    v.baseVersion = base(j.at("base_version"));
    v.pageIndex = static_cast<std::uint32_t>(num(j.at("page_index"), 63));
    auto &a = j.at("missing_ranges");
    if (!a.is_array() || a.size() > 512)
        bad();
    for (auto &r : a) {
        if (!r.is_array() || r.size() != 2)
            bad();
        v.missingRanges.push_back({static_cast<std::uint32_t>(num(r[0], 65536)),
                                   static_cast<std::uint32_t>(num(r[1], 65536))});
    }
    encodePlanPage(v);
    return v;
}
std::string encodePlanDone(const PlanDone &v) {
    if (v.changeKind != "Added" && v.changeKind != "Modified" && v.changeKind != "Unchanged")
        bad();
    if (v.pageCount > 64 || v.blockCount > 65536 || v.missingBlocks > v.blockCount ||
        v.reusedBlocks != v.blockCount - v.missingBlocks || v.missingBytes > 67108864 ||
        v.reusedBytes > 67108864 - v.missingBytes || (v.pageCount == 0) != (v.missingBlocks == 0))
        bad();
    return dump(J{{"session_id", hex(v.sessionId)},
                  {"base_version", base(v.baseVersion)},
                  {"page_count", v.pageCount},
                  {"block_count", v.blockCount},
                  {"missing_blocks", v.missingBlocks},
                  {"missing_bytes", v.missingBytes},
                  {"reused_blocks", v.reusedBlocks},
                  {"reused_bytes", v.reusedBytes},
                  {"change_kind", v.changeKind}});
}
PlanDone decodePlanDone(std::string_view s) {
    auto j = parse(s);
    fields(j, {"session_id", "base_version", "page_count", "block_count", "missing_blocks",
               "missing_bytes", "reused_blocks", "reused_bytes", "change_kind"});
    PlanDone v{id<16>(j.at("session_id")),
               base(j.at("base_version")),
               static_cast<std::uint32_t>(num(j.at("page_count"), 64)),
               static_cast<std::uint32_t>(num(j.at("block_count"), 65536)),
               num(j.at("missing_blocks")),
               num(j.at("missing_bytes")),
               num(j.at("reused_blocks")),
               num(j.at("reused_bytes")),
               str(j.at("change_kind"))};
    encodePlanDone(v);
    return v;
}
PlanValidator::PlanValidator(SessionId s, std::optional<VersionId> b, std::uint64_t z,
                             std::uint32_t c)
    : session_(s), base_(b), size_(z), chunkSize_(c), blocks_(0) {
    if (!c || c > MaxChunkDataSize || z > 67108864)
        bad();
    auto n = z / c + (z % c != 0);
    if (n > 65536)
        bad();
    blocks_ = static_cast<std::uint32_t>(n);
}
void PlanValidator::append(const PlanPage &p) {
    encodePlanPage(p);
    if (finished_ || p.sessionId != session_ || p.baseVersion != base_ || p.pageIndex != pages_ ||
        (pages_ && p.missingRanges.front().begin <= lastEnd_))
        bad();
    for (auto r : p.missingRanges) {
        if (r.end > blocks_)
            bad();
    }
    for (auto r : p.missingRanges) {
        missingBlocks_ += r.end - r.begin;
        missingBytes_ +=
            std::min<std::uint64_t>(size_, static_cast<std::uint64_t>(r.end) * chunkSize_) -
            static_cast<std::uint64_t>(r.begin) * chunkSize_;
    }
    lastEnd_ = p.missingRanges.back().end;
    ++pages_;
}
void PlanValidator::finish(const PlanDone &d) {
    encodePlanDone(d);
    if (finished_ || d.sessionId != session_ || d.baseVersion != base_ || d.pageCount != pages_ ||
        d.blockCount != blocks_ || d.missingBlocks != missingBlocks_ ||
        d.missingBytes != missingBytes_ || d.reusedBlocks != blocks_ - missingBlocks_ ||
        d.reusedBytes != size_ - missingBytes_)
        bad();
    finished_ = true;
}
} // namespace chunkharbor::protocol
