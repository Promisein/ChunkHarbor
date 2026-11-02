#pragma once
#include "chunkharbor/protocol/UploadControl.h"
#include <vector>
namespace chunkharbor::protocol {
constexpr std::uint32_t MaxFilesPerPage = 64;
struct ListFiles {
    std::string afterPath;
    std::uint32_t limit = 32;
};
struct ListedFile {
    std::string path, versionId;
    std::uint64_t size = 0;
    std::string sha256;
};
struct FilesResult {
    std::vector<ListedFile> files;
    std::string nextAfterPath;
};
// Types 11/12; JSON integers are exact, never convert file sizes through double.
std::string encodeListFiles(const ListFiles &);
ListFiles decodeListFiles(std::string_view);
std::string encodeFilesResult(const FilesResult &);
FilesResult decodeFilesResult(std::string_view);
} // namespace chunkharbor::protocol
