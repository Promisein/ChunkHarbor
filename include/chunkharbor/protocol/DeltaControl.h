#pragma once
#include "chunkharbor/protocol/BeginUpload.h"
#include <optional>
namespace chunkharbor::protocol {
using SessionId = std::array<std::uint8_t, 16>;
using VersionId = std::array<std::uint8_t, 16>;
enum class DeltaMode { Upload, Preview };
enum class DeltaPhase {
    Collecting,
    PlanReady,
    Transferring,
    Verifying,
    Committing,
    Committed,
    Failed,
    Expired
};
struct BaseSelector {
    enum class Kind { Auto, Absent, Exact };
    Kind kind = Kind::Auto;
    VersionId versionId{};
};
struct BeginDelta {
    BeginUpload metadata;
    DeltaMode mode = DeltaMode::Upload;
    BaseSelector base;
};
struct DeltaReady {
    SessionId sessionId{};
    std::optional<VersionId> baseVersion;
    std::uint32_t blockCount = 0;
};
struct ManifestDone {
    SessionId sessionId{};
    std::uint32_t pageCount = 0, blockCount = 0;
};
struct ManifestAck {
    SessionId sessionId{};
    std::uint32_t nextPageIndex = 0;
    DeltaPhase phase = DeltaPhase::Collecting;
};
struct GetPlanPage {
    SessionId sessionId{};
    std::uint32_t pageIndex = 0;
};
struct QueryDelta {
    SessionId sessionId{};
};
using CommitDelta = QueryDelta;
struct DeltaResult {
    SessionId sessionId{};
    VersionId versionId{};
    std::string path;
    std::uint64_t size = 0;
    std::string sha256;
    bool committed = true;
};
struct DeltaStatus {
    SessionId sessionId{};
    DeltaMode mode = DeltaMode::Upload;
    DeltaPhase phase = DeltaPhase::Collecting;
    std::optional<VersionId> baseVersion;
    std::uint32_t nextManifestPage = 0;
    std::string missingBitmap;
    std::optional<DeltaResult> result;
};
#define CH_DELTA_CODEC(T)                                                                          \
    std::string encode##T(const T &);                                                              \
    T decode##T(std::string_view);
CH_DELTA_CODEC(BeginDelta)
CH_DELTA_CODEC(DeltaReady) CH_DELTA_CODEC(ManifestDone) CH_DELTA_CODEC(ManifestAck)
    CH_DELTA_CODEC(GetPlanPage) CH_DELTA_CODEC(QueryDelta) CH_DELTA_CODEC(CommitDelta)
        CH_DELTA_CODEC(DeltaResult) CH_DELTA_CODEC(DeltaStatus)
#undef CH_DELTA_CODEC
} // namespace chunkharbor::protocol
