#pragma once
#include <filesystem>
#include <optional>
#include <set>
#include <vector>
namespace chunkharbor::client {
struct ScanEntry {
    std::filesystem::path localPath;
    std::string logicalPath;
    bool excluded = false;
    std::string error;
};
class DirectoryScanner {
    std::filesystem::path root_;
    std::filesystem::recursive_directory_iterator it_, end_;
    std::set<std::string> exclusions_;
    std::filesystem::path state_;
    std::size_t files_ = 0;

  public:
    DirectoryScanner(std::filesystem::path root, std::set<std::string> exclusions = {},
                     std::filesystem::path state = {});
    std::optional<ScanEntry> next();
};
} // namespace chunkharbor::client
