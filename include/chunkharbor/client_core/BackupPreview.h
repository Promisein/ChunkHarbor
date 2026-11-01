#pragma once
#include "chunkharbor/client_core/DeltaExchange.h"
namespace chunkharbor::client {
enum class ChangeKind { Added, Modified, Unchanged, Excluded, Error };
struct PreviewEntry {
    std::string logicalPath;
    SnapshotIdentity sourceIdentity;
    std::optional<protocol::VersionId> baseVersion;
    ChangeKind changeKind = ChangeKind::Error;
    std::uint64_t missingBlocks = 0, missingBytes = 0, reusedBytes = 0;
};
inline ChangeKind previewChange(const protocol::PlanDone &p) {
    if (p.changeKind == "Added")
        return ChangeKind::Added;
    if (p.changeKind == "Unchanged")
        return ChangeKind::Unchanged;
    if (p.changeKind == "Modified")
        return ChangeKind::Modified;
    throw std::runtime_error("invalid preview change kind");
}
} // namespace chunkharbor::client
