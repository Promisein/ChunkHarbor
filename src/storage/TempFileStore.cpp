#include "chunkharbor/storage/TempFileStore.h"
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <linux/openat2.h>
#include <stdexcept>
#include <string>
#include <system_error>
#include <sys/stat.h>
#include <sys/syscall.h>

namespace chunkharbor {
namespace {
[[noreturn]] void fail(const char* operation) { throw std::system_error(errno,std::generic_category(),operation); }
void validatePath(std::string_view path, bool absolute) {
    if (!absolute && path == ".") return;
    if (path.empty() || path.size() > 4096 || (path.front() == '/') != absolute)
        throw std::invalid_argument("invalid staging directory path");
    for (const unsigned char c : path)
        if (c < 32 || c == 127 || c == '\\' || c == ':')
            throw std::invalid_argument("invalid staging directory byte");
    std::size_t start = absolute ? 1 : 0;
    for (;;) {
        const auto end = path.find('/',start);
        const auto part = path.substr(start,end == std::string_view::npos ? path.size()-start : end-start);
        if (part.empty() || part.size() > 255 || part == "." || part == "..")
            throw std::invalid_argument("invalid staging directory component");
        if (end == std::string_view::npos) break;
        start = end+1;
    }
}
UniqueFd openRestricted(int parent, std::string_view path, std::uint64_t flags,
                        std::uint64_t mode, std::uint64_t resolve) {
    const std::string terminated(path);
    const open_how how{flags,mode,resolve};
    long result;
    do { result = ::syscall(SYS_openat2,parent,terminated.c_str(),&how,sizeof(how)); }
    while (result < 0 && errno == EINTR);
    // ENOSYS/EOPNOTSUPP/EAGAIN are errors; never fall back to weaker resolution.
    if (result < 0) fail("openat2 staging");
    return UniqueFd(static_cast<int>(result));
}
} // namespace
TempFileStore::TempFileStore(std::string_view absoluteRoot) {
    validatePath(absoluteRoot,true);
    root_ = openRestricted(AT_FDCWD,absoluteRoot,O_RDONLY|O_DIRECTORY|O_CLOEXEC,0,
                          RESOLVE_NO_SYMLINKS|RESOLVE_NO_MAGICLINKS);
    struct stat st{};
    if (::fstat(root_.get(),&st) < 0) fail("fstat staging root");
    if (!S_ISDIR(st.st_mode) || st.st_uid != ::geteuid() || (st.st_mode & 0077) != 0)
        throw std::system_error(EPERM,std::generic_category(),"staging root must be private and service-owned");
}
UniqueFd TempFileStore::create(std::string_view relativeDirectory) const {
    validatePath(relativeDirectory,false);
    auto file = openRestricted(root_.get(),relativeDirectory,O_TMPFILE|O_RDWR|O_CLOEXEC,0600,
                              RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS|RESOLVE_NO_MAGICLINKS|RESOLVE_NO_XDEV);
    // Restore the required private mode even under a restrictive inherited umask.
    if (::fchmod(file.get(),0600) < 0) fail("fchmod staging file");
    struct stat st{};
    if (::fstat(file.get(),&st) < 0) fail("fstat staging file");
    if (!S_ISREG(st.st_mode) || st.st_nlink != 0)
        throw std::system_error(EIO,std::generic_category(),"unexpected staging inode");
    return file;
}
} // namespace chunkharbor
