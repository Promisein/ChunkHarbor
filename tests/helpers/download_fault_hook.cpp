#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <dlfcn.h>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>
namespace {
bool mode(const char *value) {
    const char *env = std::getenv("CHUNKHARBOR_DOWNLOAD_FAULT");
    return env && std::strcmp(env, value) == 0;
}
bool temporary(int fd) {
    char name[64], path[4096];
    std::snprintf(name, sizeof name, "/proc/self/fd/%d", fd);
    auto n = ::readlink(name, path, sizeof path - 1);
    if (n < 0) return false;
    path[n] = '\0';
    return std::strstr(path, "/.chunkharbor-download-") != nullptr;
}
}
extern "C" ssize_t write(int fd, const void *data, size_t count) {
    static auto real = reinterpret_cast<ssize_t (*)(int, const void *, size_t)>(::dlsym(RTLD_NEXT, "write"));
    if (temporary(fd)) {
        if (mode("enospc")) { errno = ENOSPC; return -1; }
        if (mode("short_eintr")) {
            static int calls = 0;
            if (++calls == 1) { errno = EINTR; return -1; }
            if (calls == 2 && count > 1) count = 1;
        }
    }
    return real(fd, data, count);
}
extern "C" int fsync(int fd) {
    static auto real = reinterpret_cast<int (*)(int)>(::dlsym(RTLD_NEXT, "fsync"));
    struct stat st{};
    if ((mode("filesync") && temporary(fd)) ||
        (mode("dirsync") && ::fstat(fd, &st) == 0 && S_ISDIR(st.st_mode))) {
        errno = EIO;
        return -1;
    }
    return real(fd);
}
