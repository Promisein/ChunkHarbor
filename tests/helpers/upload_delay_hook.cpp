// Test-only slow durability boundary; never linked into the server.
#include <cerrno>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
namespace {
long setting(const char *name, long fallback, long maximum) {
    const char *text = std::getenv(name);
    if (!text) return fallback;
    char *end = nullptr;
    const long value = std::strtol(text, &end, 10);
    return *text && !*end && value >= 0 && value <= maximum ? value : fallback;
}
}
extern "C" int fdatasync(int fd) {
    using Fn = int (*)(int);
    static auto real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "fdatasync"));
    const char *marker = std::getenv("CH_UPLOAD_DELAY_MARKER");
    char link[64], path[4096];
    std::snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    const auto n = ::readlink(link, path, sizeof path - 1);
    if (n >= 0) path[n] = '\0';
    static std::atomic_long calls{0};
    if (marker && n >= 0 && std::strstr(path, "/.delta/") &&
        calls.fetch_add(1) >= setting("CH_UPLOAD_DELAY_SKIP", 0, 65536)) {
        const int created = ::open(marker, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
        if (created >= 0) {
            ::close(created);
            const auto ms = setting("CH_UPLOAD_DELAY_MS", 2000, 45000);
            timespec delay{ms / 1000, (ms % 1000) * 1000000};
            while (::nanosleep(&delay, &delay) < 0 && errno == EINTR) {}
        }
    }
    return real(fd);
}

extern "C" int fsync(int fd) {
    using Fn = int (*)(int);
    static auto real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "fsync"));
    const char *marker = std::getenv("CH_LEGACY_VERSION_SYNC_MARKER");
    const char *release = std::getenv("CH_LEGACY_VERSION_SYNC_RELEASE");
    if (marker && release) {
        char link[64], path[4096];
        std::snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
        const auto n = ::readlink(link, path, sizeof path - 1);
        if (n >= 0) {
            path[n] = '\0';
            if (std::strstr(path, "/.versions/") && std::strstr(path, "/pending")) {
                const int ready = ::open(marker, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
                if (ready >= 0) {
                    ::close(ready);
                    const timespec delay{0, 10000000};
                    for (int attempt = 0; attempt < 1000 && ::access(release, F_OK) != 0; ++attempt)
                        ::nanosleep(&delay, nullptr);
                }
            }
        }
    }
    return real(fd);
}
