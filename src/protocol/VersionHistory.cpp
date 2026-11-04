#include "chunkharbor/protocol/VersionHistory.h"
#include "DeltaJson.h"

namespace chunkharbor::protocol {
namespace {
using namespace delta_json;
void validateFile(const std::string &path, std::uint64_t size, const std::string &sha) {
    try { encodeBeginUpload({path, size, 0, 1, sha}); }
    catch (const BeginUploadError &) { bad(); }
}
J writeOptional(const std::optional<std::uint64_t> &value) { return value ? J(*value) : J(nullptr); }
std::optional<std::uint64_t> readOptional(const J &value) {
    if (value.is_null()) return {};
    return num(value, INT64_MAX);
}
void validateCursor(const std::optional<std::uint64_t> &value) {
    if (value && (!*value || *value > INT64_MAX)) bad();
}
}

std::string encodeListVersions(const ListVersions &value) {
    validateFile(value.path, 0, std::string(64, '0'));
    validateCursor(value.before);
    if (!value.limit || value.limit > MaxVersionsPerPage) bad();
    return dump(J{{"path", value.path}, {"before", writeOptional(value.before)}, {"limit", value.limit}});
}
ListVersions decodeListVersions(std::string_view payload) {
    const auto j = parse(payload);
    fields(j, {"path", "before", "limit"});
    ListVersions value{str(j.at("path")), readOptional(j.at("before")),
                       static_cast<std::uint32_t>(num(j.at("limit"), MaxVersionsPerPage))};
    encodeListVersions(value);
    return value;
}
std::string encodeVersionsResult(const VersionsResult &value) {
    validateFile(value.path, 0, std::string(64, '0'));
    validateCursor(value.nextBefore);
    if (value.versions.size() > MaxVersionsPerPage ||
        (value.nextBefore && (value.versions.empty() || *value.nextBefore != value.versions.back().sequence)))
        bad();
    J items = J::array();
    std::uint64_t previous = static_cast<std::uint64_t>(INT64_MAX) + 1;
    std::set<std::string> ids;
    bool seenCurrent = false;
    for (const auto &row : value.versions) {
        validateFile(value.path, row.size, row.sha256);
        id<16>(J(row.versionId));
        if (!row.sequence || row.sequence >= previous || !ids.insert(row.versionId).second ||
            (row.current && seenCurrent) || (row.committedAtSeconds && *row.committedAtSeconds > INT64_MAX))
            bad();
        previous = row.sequence;
        seenCurrent |= row.current;
        items.push_back(J{{"version_id", row.versionId}, {"size", row.size}, {"sha256", row.sha256},
                          {"sequence", row.sequence}, {"committed_at_s", writeOptional(row.committedAtSeconds)},
                          {"current", row.current}});
    }
    return dump(J{{"path", value.path}, {"versions", items}, {"next_before", writeOptional(value.nextBefore)}});
}
VersionsResult decodeVersionsResult(std::string_view payload) {
    const auto j = parse(payload);
    fields(j, {"path", "versions", "next_before"});
    const auto &items = j.at("versions");
    if (!items.is_array() || items.size() > MaxVersionsPerPage) bad();
    VersionsResult value{str(j.at("path")), {}, readOptional(j.at("next_before"))};
    for (const auto &row : items) {
        fields(row, {"version_id", "size", "sha256", "sequence", "committed_at_s", "current"});
        if (!row.at("current").is_boolean()) bad();
        value.versions.push_back({str(row.at("version_id")), num(row.at("size")), str(row.at("sha256")),
                                  num(row.at("sequence"), INT64_MAX), readOptional(row.at("committed_at_s")),
                                  row.at("current").get<bool>()});
    }
    encodeVersionsResult(value);
    return value;
}
}
