#include "chunkharbor/protocol/DirectorySnapshot.h"
#include "chunkharbor/protocol/Download.h"
#include "DeltaJson.h"

namespace chunkharbor::protocol {
namespace {
using namespace delta_json;
void validatePath(const std::string &path, std::uint64_t size = 0,
                  const std::string &sha = std::string(64, '0')) {
    try { encodeBeginUpload({path, size, 0, 1, sha}); }
    catch (const BeginUploadError &) { bad(); }
}
void validateCursor(const std::optional<std::uint64_t> &value) {
    if (value && (!*value || *value > INT64_MAX)) bad();
}
J cursor(const std::optional<std::uint64_t> &value) { return value ? J(*value) : J(nullptr); }
std::optional<std::uint64_t> readCursor(const J &value) {
    if (value.is_null()) return {};
    return num(value, INT64_MAX);
}
J header(const SnapshotInfo &value) {
    id<16>(J(value.id));
    if (!value.sequence || value.sequence > INT64_MAX || value.createdAtSeconds > INT64_MAX ||
        value.fileCount > MaxSnapshotFiles) bad();
    return {{"snapshot_id", value.id}, {"sequence", value.sequence}, {"created_at_s", value.createdAtSeconds},
            {"file_count", value.fileCount}, {"state", "committed"}};
}
SnapshotInfo readHeader(const J &j, const std::string &root) {
    fields(j, {"snapshot_id", "sequence", "created_at_s", "file_count", "state"});
    if (str(j.at("state")) != "committed") bad();
    return {str(j.at("snapshot_id")), root, num(j.at("sequence"), INT64_MAX),
            num(j.at("created_at_s"), INT64_MAX), static_cast<std::uint32_t>(num(j.at("file_count"), MaxSnapshotFiles))};
}
}
std::string encodeCreateSnapshot(const std::string &root) {
    validatePath(root);
    return dump(J{{"root", root}});
}
std::string decodeCreateSnapshot(std::string_view payload) {
    const auto j = parse(payload);
    fields(j, {"root"});
    auto root = str(j.at("root"));
    validatePath(root);
    return root;
}
std::string encodeGetSnapshot(const std::string &value) {
    id<16>(J(value));
    return dump(J{{"snapshot_id", value}});
}
std::string decodeGetSnapshot(std::string_view payload) {
    const auto j = parse(payload);
    fields(j, {"snapshot_id"});
    id<16>(j.at("snapshot_id"));
    return str(j.at("snapshot_id"));
}
std::string encodeSnapshotInfo(const SnapshotInfo &value) {
    validatePath(value.root);
    auto j = header(value);
    j["root"] = value.root;
    return dump(j);
}
SnapshotInfo decodeSnapshotInfo(std::string_view payload) {
    auto j = parse(payload);
    if (!j.is_object() || !j.contains("root")) bad();
    const auto root = str(j.at("root"));
    j.erase("root");
    auto value = readHeader(j, root);
    encodeSnapshotInfo(value);
    return value;
}
std::string encodeListSnapshots(const ListSnapshots &value) {
    validatePath(value.root);
    validateCursor(value.before);
    if (!value.limit || value.limit > MaxSnapshotsPerPage) bad();
    return dump(J{{"root", value.root}, {"before", cursor(value.before)}, {"limit", value.limit}});
}
ListSnapshots decodeListSnapshots(std::string_view payload) {
    const auto j = parse(payload);
    fields(j, {"root", "before", "limit"});
    ListSnapshots value{str(j.at("root")), readCursor(j.at("before")),
                        static_cast<std::uint32_t>(num(j.at("limit"), MaxSnapshotsPerPage))};
    encodeListSnapshots(value);
    return value;
}
std::string encodeSnapshotsPage(const SnapshotsPage &value) {
    validatePath(value.root);
    validateCursor(value.nextBefore);
    if (value.snapshots.size() > MaxSnapshotsPerPage || (value.nextBefore &&
        (value.snapshots.empty() || *value.nextBefore != value.snapshots.back().sequence))) bad();
    auto rows = J::array();
    std::uint64_t previous = static_cast<std::uint64_t>(INT64_MAX) + 1;
    for (const auto &item : value.snapshots) {
        if (item.root != value.root || item.sequence >= previous) bad();
        rows.push_back(header(item));
        previous = item.sequence;
    }
    return dump(J{{"root", value.root}, {"snapshots", rows}, {"next_before", cursor(value.nextBefore)}});
}
SnapshotsPage decodeSnapshotsPage(std::string_view payload) {
    const auto j = parse(payload);
    fields(j, {"root", "snapshots", "next_before"});
    const auto &rows = j.at("snapshots");
    if (!rows.is_array() || rows.size() > MaxSnapshotsPerPage) bad();
    SnapshotsPage value{str(j.at("root")), {}, readCursor(j.at("next_before"))};
    for (const auto &item : rows) value.snapshots.push_back(readHeader(item, value.root));
    encodeSnapshotsPage(value);
    return value;
}
std::string encodeListSnapshotEntries(const ListSnapshotEntries &value) {
    id<16>(J(value.snapshotId));
    if (!value.afterPath.empty()) validatePath(value.afterPath);
    if (!value.limit || value.limit > MaxSnapshotEntriesPerPage) bad();
    return dump(J{{"snapshot_id", value.snapshotId}, {"after_path", value.afterPath}, {"limit", value.limit}});
}
ListSnapshotEntries decodeListSnapshotEntries(std::string_view payload) {
    const auto j = parse(payload);
    fields(j, {"snapshot_id", "after_path", "limit"});
    ListSnapshotEntries value{str(j.at("snapshot_id")), str(j.at("after_path")),
                               static_cast<std::uint32_t>(num(j.at("limit"), MaxSnapshotEntriesPerPage))};
    encodeListSnapshotEntries(value);
    return value;
}
std::string encodeSnapshotEntriesPage(const SnapshotEntriesPage &value) {
    id<16>(J(value.snapshotId));
    if (value.entries.size() > MaxSnapshotEntriesPerPage || (!value.nextAfterPath.empty() &&
        (value.entries.empty() || value.nextAfterPath != value.entries.back().path))) bad();
    auto rows = J::array();
    std::string previous;
    for (const auto &item : value.entries) {
        validatePath(item.path, item.size, item.sha256);
        id<16>(J(item.versionId));
        if (item.size > MaxDownloadFileSize || item.path <= previous) bad();
        rows.push_back(J{{"path", item.path}, {"version_id", item.versionId}, {"size", item.size}, {"sha256", item.sha256}});
        previous = item.path;
    }
    return dump(J{{"snapshot_id", value.snapshotId}, {"entries", rows}, {"next_after_path", value.nextAfterPath}});
}
SnapshotEntriesPage decodeSnapshotEntriesPage(std::string_view payload) {
    const auto j = parse(payload);
    fields(j, {"snapshot_id", "entries", "next_after_path"});
    const auto &rows = j.at("entries");
    if (!rows.is_array() || rows.size() > MaxSnapshotEntriesPerPage) bad();
    SnapshotEntriesPage value{str(j.at("snapshot_id")), {}, str(j.at("next_after_path"))};
    for (const auto &item : rows) {
        fields(item, {"path", "version_id", "size", "sha256"});
        value.entries.push_back({str(item.at("path")), str(item.at("version_id")),
                                 num(item.at("size"), MaxDownloadFileSize), str(item.at("sha256"))});
    }
    encodeSnapshotEntriesPage(value);
    return value;
}
}
