#pragma once
#include "chunkharbor/common/UniqueFd.h"
#include <string_view>

namespace chunkharbor {
// Linux >=5.6, O_TMPFILE filesystem. Root must exist, be owned by this euid,
// and have no group/other permission bits. No symlinks in its configured path.
class TempFileStore {
public:
    explicit TempFileStore(std::string_view absoluteRoot);
    TempFileStore(TempFileStore&&) noexcept = default;
    TempFileStore& operator=(TempFileStore&&) noexcept = default;
    TempFileStore(const TempFileStore&) = delete;
    TempFileStore& operator=(const TempFileStore&) = delete;
    // Directory is an internal Linux root-relative directory, NOT a destination
    // filename. It must already exist. "." selects the pinned root itself.
    // Returns an owned, unnamed, empty 0600 regular file. No I/O persistence or
    // publication is implied. Last close discards data unless explicitly linked.
    UniqueFd create(std::string_view relativeDirectory = ".") const;
private:
    UniqueFd root_;
};
} // namespace chunkharbor
