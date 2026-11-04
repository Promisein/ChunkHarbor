#pragma once
#include "chunkharbor/protocol/FilesList.h"
#include <optional>

namespace chunkharbor::protocol {
inline constexpr char VersionHistoryCapability[] = "version_history_v1";
inline constexpr std::uint16_t ListVersionsType = 34, VersionsResultType = 35;
inline constexpr std::uint32_t MaxVersionsPerPage = 64;

struct ListVersions {
    std::string path;
    std::optional<std::uint64_t> before;
    std::uint32_t limit = 32;
};
struct HistoryVersion {
    std::string versionId;
    std::uint64_t size;
    std::string sha256;
    std::uint64_t sequence;
    std::optional<std::uint64_t> committedAtSeconds;
    bool current;
};
struct VersionsResult {
    std::string path;
    std::vector<HistoryVersion> versions;
    std::optional<std::uint64_t> nextBefore;
};
std::string encodeListVersions(const ListVersions &);
ListVersions decodeListVersions(std::string_view);
std::string encodeVersionsResult(const VersionsResult &);
VersionsResult decodeVersionsResult(std::string_view);
}
