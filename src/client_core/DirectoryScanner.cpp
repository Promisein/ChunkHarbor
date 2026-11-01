#include "chunkharbor/client_core/DirectoryScanner.h"
#include <stdexcept>
namespace chunkharbor::client {
DirectoryScanner::DirectoryScanner(std::filesystem::path root, std::set<std::string> exclusions,
                                   std::filesystem::path state)
    : root_(std::filesystem::absolute(root).lexically_normal()), exclusions_(std::move(exclusions)),
      state_(state.empty() ? state : std::filesystem::absolute(state).lexically_normal()) {
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(root_)) ||
        !std::filesystem::is_directory(root_))
        throw std::runtime_error("source root must be a directory, not link");
    it_ = std::filesystem::recursive_directory_iterator(root_);
}
std::optional<ScanEntry> DirectoryScanner::next() {
    while (it_ != end_) {
        auto entry = *it_;
        auto depth = it_.depth();
        auto status = entry.symlink_status();
        auto local = entry.path();
        auto relative = local.lexically_relative(root_).generic_u8string();
        if (depth >= 64)
            throw std::runtime_error("directory depth exceeds 64");
        if (std::filesystem::is_symlink(status) ||
            std::filesystem::weakly_canonical(local) !=
                std::filesystem::weakly_canonical(root_) / local.lexically_relative(root_)) {
            if (std::filesystem::is_directory(status))
                it_.disable_recursion_pending();
            ++it_;
            return ScanEntry{local, relative, false, "source symbolic link/reparse rejected"};
        }
        bool excluded = local.lexically_normal() == state_ ||
                        (std::filesystem::is_directory(status) &&
                         exclusions_.count(local.filename().u8string()) != 0);
        if (excluded) {
            if (std::filesystem::is_directory(status))
                it_.disable_recursion_pending();
            ++it_;
            return ScanEntry{local, relative, true, {}};
        }
        if (std::filesystem::is_directory(status)) {
            ++it_;
            continue;
        }
        if (!std::filesystem::is_regular_file(status))
            throw std::runtime_error("source special file rejected: " + relative);
        if (++files_ > 10000)
            throw std::runtime_error("directory exceeds 10000 files");
        if (entry.file_size() > 64ULL * 1024 * 1024)
            throw std::runtime_error("source exceeds 64 MiB: " + relative);
        ++it_;
        return ScanEntry{local, relative, false, {}};
    }
    return {};
}
} // namespace chunkharbor::client
