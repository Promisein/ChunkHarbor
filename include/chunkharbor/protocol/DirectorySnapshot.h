#pragma once
#include "chunkharbor/protocol/FilesList.h"
#include <optional>

namespace chunkharbor::protocol {
inline constexpr char DirectorySnapshotCapability[] = "directory_snapshot_v1";
enum SnapshotFrame : std::uint16_t {
    CreateSnapshotType = 37, GetSnapshotType = 38, SnapshotInfoType = 39,
    ListSnapshotsType = 40, SnapshotsPageType = 41,
    ListSnapshotEntriesType = 42, SnapshotEntriesPageType = 43
};
inline constexpr std::uint32_t MaxSnapshotFiles = 10000;
inline constexpr std::uint32_t MaxSnapshotsPerPage = 64;
// Six maximally escaped 4096-byte paths plus the cursor fit in a 64 KiB frame.
inline constexpr std::uint32_t MaxSnapshotEntriesPerPage = 6;

struct SnapshotInfo {
    std::string id, root;
    std::uint64_t sequence = 0, createdAtSeconds = 0;
    std::uint32_t fileCount = 0;
};
struct ListSnapshots {
    std::string root;
    std::optional<std::uint64_t> before;
    std::uint32_t limit = 32;
};
struct SnapshotsPage {
    std::string root;
    std::vector<SnapshotInfo> snapshots;
    std::optional<std::uint64_t> nextBefore;
};
struct ListSnapshotEntries {
    std::string snapshotId, afterPath;
    std::uint32_t limit = MaxSnapshotEntriesPerPage;
};
struct SnapshotEntriesPage {
    std::string snapshotId;
    std::vector<ListedFile> entries;
    std::string nextAfterPath;
};

std::string encodeCreateSnapshot(const std::string &root);
std::string decodeCreateSnapshot(std::string_view);
std::string encodeGetSnapshot(const std::string &id);
std::string decodeGetSnapshot(std::string_view);
std::string encodeSnapshotInfo(const SnapshotInfo &);
SnapshotInfo decodeSnapshotInfo(std::string_view);
std::string encodeListSnapshots(const ListSnapshots &);
ListSnapshots decodeListSnapshots(std::string_view);
std::string encodeSnapshotsPage(const SnapshotsPage &);
SnapshotsPage decodeSnapshotsPage(std::string_view);
std::string encodeListSnapshotEntries(const ListSnapshotEntries &);
ListSnapshotEntries decodeListSnapshotEntries(std::string_view);
std::string encodeSnapshotEntriesPage(const SnapshotEntriesPage &);
SnapshotEntriesPage decodeSnapshotEntriesPage(std::string_view);
}
