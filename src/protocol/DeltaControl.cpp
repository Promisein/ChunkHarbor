#include "chunkharbor/protocol/DeltaControl.h"
#include "DeltaJson.h"
namespace chunkharbor::protocol {
namespace {
using namespace delta_json;
const char *mode(DeltaMode m) {
    switch (m) {
    case DeltaMode::Upload:
        return "upload";
    case DeltaMode::Preview:
        return "preview";
    }
    bad();
}
DeltaMode mode(const J &j) {
    auto s = str(j);
    if (s == "upload")
        return DeltaMode::Upload;
    if (s == "preview")
        return DeltaMode::Preview;
    bad();
}
const char *phase(DeltaPhase p) {
    switch (p) {
    case DeltaPhase::Collecting:
        return "COLLECTING";
    case DeltaPhase::PlanReady:
        return "PLAN_READY";
    case DeltaPhase::Transferring:
        return "TRANSFERRING";
    case DeltaPhase::Verifying:
        return "VERIFYING";
    case DeltaPhase::Committing:
        return "COMMITTING";
    case DeltaPhase::Committed:
        return "COMMITTED";
    case DeltaPhase::Failed:
        return "FAILED";
    case DeltaPhase::Expired:
        return "EXPIRED";
    }
    bad();
}
DeltaPhase phase(const J &j) {
    auto s = str(j);
    for (auto p : {DeltaPhase::Collecting, DeltaPhase::PlanReady, DeltaPhase::Transferring,
                   DeltaPhase::Verifying, DeltaPhase::Committing, DeltaPhase::Committed,
                   DeltaPhase::Failed, DeltaPhase::Expired})
        if (s == phase(p))
            return p;
    bad();
}
} // namespace
std::string encodeBeginDelta(const BeginDelta &v) {
    J b;
    switch (v.base.kind) {
    case BaseSelector::Kind::Auto:
        b = "auto";
        break;
    case BaseSelector::Kind::Absent:
        b = nullptr;
        break;
    case BaseSelector::Kind::Exact:
        b = hex(v.base.versionId);
        break;
    default:
        bad();
    }
    return dump(J{{"metadata", J::parse(encodeBeginUpload(v.metadata))},
                  {"mode", mode(v.mode)},
                  {"base_version", b}});
}
BeginDelta decodeBeginDelta(std::string_view s) {
    auto j = parse(s);
    fields(j, {"metadata", "mode", "base_version"});
    BeginDelta v;
    v.metadata = decodeBeginUpload(j.at("metadata").dump());
    v.mode = mode(j.at("mode"));
    auto b = j.at("base_version");
    if (b.is_null())
        v.base.kind = BaseSelector::Kind::Absent;
    else if (b == "auto")
        v.base.kind = BaseSelector::Kind::Auto;
    else {
        v.base.kind = BaseSelector::Kind::Exact;
        v.base.versionId = id<16>(b);
    }
    return v;
}
std::string encodeDeltaReady(const DeltaReady &v) {
    if (v.blockCount > 65536)
        bad();
    return dump(J{{"session_id", hex(v.sessionId)},
                  {"base_version", base(v.baseVersion)},
                  {"block_count", v.blockCount}});
}
DeltaReady decodeDeltaReady(std::string_view s) {
    auto j = parse(s);
    fields(j, {"session_id", "base_version", "block_count"});
    return {id<16>(j.at("session_id")), base(j.at("base_version")),
            static_cast<std::uint32_t>(num(j.at("block_count"), 65536))};
}
std::string encodeManifestDone(const ManifestDone &v) {
    if (v.blockCount > 65536 || v.pageCount != (v.blockCount + 511) / 512)
        bad();
    return dump(J{{"session_id", hex(v.sessionId)},
                  {"page_count", v.pageCount},
                  {"block_count", v.blockCount}});
}
ManifestDone decodeManifestDone(std::string_view s) {
    auto j = parse(s);
    fields(j, {"session_id", "page_count", "block_count"});
    ManifestDone v{id<16>(j.at("session_id")),
                   static_cast<std::uint32_t>(num(j.at("page_count"), 128)),
                   static_cast<std::uint32_t>(num(j.at("block_count"), 65536))};
    encodeManifestDone(v);
    return v;
}
std::string encodeManifestAck(const ManifestAck &v) {
    if (v.nextPageIndex > 128 ||
        (v.phase != DeltaPhase::Collecting && v.phase != DeltaPhase::PlanReady))
        bad();
    return dump(J{{"session_id", hex(v.sessionId)},
                  {"next_page_index", v.nextPageIndex},
                  {"phase", phase(v.phase)}});
}
ManifestAck decodeManifestAck(std::string_view s) {
    auto j = parse(s);
    fields(j, {"session_id", "next_page_index", "phase"});
    ManifestAck v{id<16>(j.at("session_id")),
                  static_cast<std::uint32_t>(num(j.at("next_page_index"), 128)),
                  phase(j.at("phase"))};
    encodeManifestAck(v);
    return v;
}
std::string encodeGetPlanPage(const GetPlanPage &v) {
    if (v.pageIndex > 64)
        bad();
    return dump(J{{"session_id", hex(v.sessionId)}, {"page_index", v.pageIndex}});
}
GetPlanPage decodeGetPlanPage(std::string_view s) {
    auto j = parse(s);
    fields(j, {"session_id", "page_index"});
    return {id<16>(j.at("session_id")), static_cast<std::uint32_t>(num(j.at("page_index"), 64))};
}
std::string encodeQueryDelta(const QueryDelta &v) {
    return dump(J{{"session_id", hex(v.sessionId)}});
}
QueryDelta decodeQueryDelta(std::string_view s) {
    auto j = parse(s);
    fields(j, {"session_id"});
    return {id<16>(j.at("session_id"))};
}
std::string encodeCommitDelta(const CommitDelta &v) { return encodeQueryDelta(v); }
CommitDelta decodeCommitDelta(std::string_view s) { return decodeQueryDelta(s); }
std::string encodeDeltaResult(const DeltaResult &v) {
    if (!v.committed)
        bad();
    encodeBeginUpload({v.path, v.size, 0, 262144, v.sha256});
    return dump(J{{"session_id", hex(v.sessionId)},
                  {"version_id", hex(v.versionId)},
                  {"path", v.path},
                  {"size", v.size},
                  {"sha256", v.sha256},
                  {"committed", true}});
}
DeltaResult decodeDeltaResult(std::string_view s) {
    auto j = parse(s);
    fields(j, {"session_id", "version_id", "path", "size", "sha256", "committed"});
    if (!j.at("committed").is_boolean() || !j.at("committed").get<bool>())
        bad();
    DeltaResult v{id<16>(j.at("session_id")), id<16>(j.at("version_id")), str(j.at("path")),
                  num(j.at("size")),          str(j.at("sha256")),        true};
    encodeDeltaResult(v);
    return v;
}
std::string encodeDeltaStatus(const DeltaStatus &v) {
    if (v.nextManifestPage > 128 || (v.phase == DeltaPhase::Committed) != v.result.has_value())
        bad();
    if (v.result && v.result->sessionId != v.sessionId)
        bad();
    bool active = v.phase == DeltaPhase::Transferring || v.phase == DeltaPhase::Verifying;
    if (!active && !v.missingBitmap.empty())
        bad();
    if (v.mode == DeltaMode::Preview &&
        (active || v.phase == DeltaPhase::Committing || v.phase == DeltaPhase::Committed))
        bad();
    if (v.missingBitmap.size() > 16384 || v.missingBitmap.size() % 2)
        bad();
    for (char c : v.missingBitmap)
        if (std::string_view("0123456789abcdef").find(c) == std::string_view::npos)
            bad();
    return dump(J{{"session_id", hex(v.sessionId)},
                  {"mode", mode(v.mode)},
                  {"phase", phase(v.phase)},
                  {"base_version", base(v.baseVersion)},
                  {"next_manifest_page", v.nextManifestPage},
                  {"missing_bitmap", v.missingBitmap},
                  {"result", v.result ? J::parse(encodeDeltaResult(*v.result)) : J(nullptr)}});
}
DeltaStatus decodeDeltaStatus(std::string_view s) {
    auto j = parse(s);
    fields(j, {"session_id", "mode", "phase", "base_version", "next_manifest_page",
               "missing_bitmap", "result"});
    DeltaStatus v;
    v.sessionId = id<16>(j.at("session_id"));
    v.mode = mode(j.at("mode"));
    v.phase = phase(j.at("phase"));
    v.baseVersion = base(j.at("base_version"));
    v.nextManifestPage = static_cast<std::uint32_t>(num(j.at("next_manifest_page"), 128));
    v.missingBitmap = str(j.at("missing_bitmap"));
    if (!j.at("result").is_null())
        v.result = decodeDeltaResult(j.at("result").dump());
    encodeDeltaStatus(v);
    return v;
}
} // namespace chunkharbor::protocol
