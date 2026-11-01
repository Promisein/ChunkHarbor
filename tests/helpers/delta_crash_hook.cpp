// Test-only LD_PRELOAD hook: stop at a durable publication boundary. The parent
// kills this stopped test process; no hook is linked into the product binary.
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
namespace {
bool chosen(const char *stage) {
    const char *value = std::getenv("CH_M4_TEST_CRASH_STAGE");
    return value && std::strcmp(value, stage) == 0;
}
bool fault(const char *stage) {
    const char *value = std::getenv("CH_M4_TEST_FAIL_STAGE");
    return value && std::strcmp(value, stage) == 0;
}
bool directoryContains(int fd, const char *text) {
    char link[64], target[4096];
    std::snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    auto length = ::readlink(link, target, sizeof target - 1);
    if (length < 0)
        return false;
    target[length] = '\0';
    return std::strstr(target, text) != nullptr;
}
void stopHere() {
    const char *marker = std::getenv("CH_M4_TEST_CRASH_MARKER");
    if (!marker)
        return;
    int fd = ::open(marker, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    if (fd < 0)
        return;
    (void)::write(fd, "ready", 5);
    ::close(fd);
    ::raise(SIGSTOP);
}
} // namespace
extern "C" int renameat(int from, const char *oldName, int to, const char *newName) {
    using Fn = int (*)(int, const char *, int, const char *);
    static auto real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "renameat"));
    const bool latest = std::strncmp(oldName, ".publish-", 9) == 0;
    if (latest && chosen("before_latest_rename"))
        stopHere();
    auto result = real(from, oldName, to, newName);
    if (result == 0 && latest && chosen("after_latest_rename"))
        stopHere();
    return result;
}
extern "C" int fsync(int fd) {
    using Fn = int (*)(int);
    static auto real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "fsync"));
    auto result = real(fd);
    struct stat info{};
    if (result == 0 && ((fault("object_sync") && directoryContains(fd, "/.versions/")) ||
                        (fault("publish_sync") && ::fstat(fd, &info) == 0 &&
                         S_ISDIR(info.st_mode) && directoryContains(fd, "/files")))) {
        errno = EIO;
        return -1;
    }
    if (result == 0 && chosen("object_synced") && directoryContains(fd, "/.versions/"))
        stopHere();
    return result;
}
extern "C" ssize_t pwrite(int fd, const void *bytes, size_t count, off_t offset) {
    using Fn = ssize_t (*)(int, const void *, size_t, off_t);
    static auto real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "pwrite"));
    if (fault("data_write") && directoryContains(fd, "/.delta/")) {
        errno = ENOSPC;
        return -1;
    }
    return real(fd, bytes, count, offset);
}
extern "C" int fdatasync(int fd) {
    using Fn = int (*)(int);
    static auto real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "fdatasync"));
    if (fault("data_sync") && directoryContains(fd, "/.delta/")) {
        errno = EIO;
        return -1;
    }
    return real(fd);
}
extern "C" int unlinkat(int fd, const char *name, int flags) {
    using Fn = int (*)(int, const char *, int);
    static auto real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "unlinkat"));
    const bool delta = directoryContains(fd, "/.delta");
    auto result = real(fd, name, flags);
    if (result == 0 && delta && chosen("after_committed_cleanup"))
        stopHere();
    return result;
}
