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
#include <time.h>
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
bool ownerStage(const char *stage) {
    const char *path = std::getenv("CH_OWNER_TEST_STAGE");
    if (!path) return false;
    char text[64]{};
    int file = ::open(path, O_RDONLY | O_CLOEXEC);
    auto length = file < 0 ? -1 : ::read(file, text, sizeof(text) - 1);
    if (file >= 0) ::close(file);
    return length > 0 && std::strcmp(text, stage) == 0;
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
    if (const auto *release = std::getenv("CH_SNAPSHOT_TEST_RELEASE")) {
        for (int i = 0; i < 1000 && ::access(release, F_OK) != 0; ++i)
            ::usleep(10000);
    } else {
        ::raise(SIGSTOP);
    }
}
bool materializeFault(int fd, const char *stage) {
    const char *gate = std::getenv("CH_MATERIALIZE_TEST_GATE");
    if (!gate || !directoryContains(fd, "/.delta/")) return false;
    char selected[128]{}, marker[4096];
    int input = ::open(gate, O_RDONLY | O_CLOEXEC);
    auto length = input < 0 ? -1 : ::read(input, selected, sizeof(selected) - 1);
    if (input >= 0) ::close(input);
    std::snprintf(marker, sizeof marker, "%s.%s", gate, stage);
    return length > 0 && directoryContains(fd, selected) && ::access(marker, F_OK) == 0;
}
bool commitBarrier(int fd, const char *operation) {
    const char *gate = std::getenv("CH_COMMIT_TEST_GATE");
    if (!gate) return false;
    char config[512]{}, marker[4096], release[4096], failure[4096];
    int input = ::open(gate, O_RDONLY | O_CLOEXEC);
    auto length = input < 0 ? -1 : ::read(input, config, sizeof(config) - 1);
    if (input >= 0) ::close(input);
    auto *path = std::strchr(config, '\n');
    if (length <= 0 || !path) return false;
    *path++ = '\0';
    if (std::strcmp(config, operation) != 0 || !directoryContains(fd, path)) return false;
    std::snprintf(marker, sizeof marker, "%s.entered", gate);
    std::snprintf(release, sizeof release, "%s.release", gate);
    std::snprintf(failure, sizeof failure, "%s.fail", gate);
    int output = ::open(marker, O_CREAT | O_APPEND | O_WRONLY | O_CLOEXEC, 0600);
    if (output >= 0) { (void)::write(output, "x", 1); ::close(output); }
    for (int n = 0; n < 1000 && ::access(release, F_OK) != 0; ++n) ::usleep(10000);
    return ::access(release, F_OK) != 0 || ::access(failure, F_OK) == 0;
}
} // namespace
extern "C" int mkdirat(int fd, const char *name, mode_t mode) {
    using Fn = int (*)(int, const char *, mode_t);
    static auto real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "mkdirat"));
    const auto result = real(fd, name, mode);
    if (result == 0 && directoryContains(fd, "/.versions") && ownerStage("object_created")) stopHere();
    return result;
}
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
    if (commitBarrier(fd, "fsync")) { errno = EIO; return -1; }
    if (materializeFault(fd, "sync_error")) { errno = EIO; return -1; }
    auto result = real(fd);
    struct stat info{};
    if (result == 0 && ownerStage("journal_durable") && directoryContains(fd, "/.preparations") &&
        ::fstat(fd, &info) == 0 && S_ISDIR(info.st_mode)) stopHere();
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
    if (commitBarrier(fd, "pwrite")) { errno = ENOSPC; return -1; }
    if (materializeFault(fd, "enospc")) { errno = ENOSPC; return -1; }
    if (fault("data_write") && directoryContains(fd, "/.delta/")) {
        errno = ENOSPC;
        return -1;
    }
    const auto result = real(fd, bytes, count, offset);
    if (result > 0 && directoryContains(fd, "/.versions/") && ownerStage("object_written")) stopHere();
    return result;
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
    const char *ownedGate = std::getenv("CH_OWNER_CLEANUP_GATE");
    if (ownedGate) {
        char id[64]{}, marker[4096];
        int input = ::open(ownedGate, O_RDONLY | O_CLOEXEC);
        auto length = input < 0 ? -1 : ::read(input, id, sizeof(id) - 1);
        if (input >= 0) ::close(input);
        std::snprintf(marker, sizeof marker, "%s.fail", ownedGate);
        if (length > 0 && directoryContains(fd, id) && ::access(marker, F_OK) == 0) {
            errno = EIO; return -1;
        }
    }
    const char *gate = std::getenv("CH_CLEANUP_TEST_GATE");
    if (gate && directoryContains(fd, "/.delta")) {
        char id[64]{}, marker[4096];
        int input = ::open(gate, O_RDONLY | O_CLOEXEC);
        auto length = input < 0 ? -1 : ::read(input, id, sizeof(id) - 1);
        if (input >= 0) ::close(input);
        std::snprintf(marker, sizeof marker, "%s.fail", gate);
        if (length > 0 && (std::strcmp(name, id) == 0 ||
            (std::strncmp(name, ".publish-", 9) == 0 && std::strcmp(name + 9, id) == 0)) &&
            ::access(marker, F_OK) == 0) { errno = EIO; return -1; }
    }
    const bool delta = directoryContains(fd, "/.delta");
    auto result = real(fd, name, flags);
    if (result == 0 && flags == AT_REMOVEDIR && directoryContains(fd, "/.versions") &&
        ownerStage("reclaim_removed")) stopHere();
    if (result == 0 && delta && chosen("after_committed_cleanup"))
        stopHere();
    return result;
}

// Optional, test-only transaction timing; no SQL text or user payload is logged.
// BEGIN/COMMIT use sqlite3_exec, while DStmt inserts use sqlite3_step.
namespace {
thread_local bool costActive = false, costLegacy = false;
thread_local timespec costStarted{};
}
struct sqlite3;
extern "C" int sqlite3_exec(sqlite3 *db, const char *statement,
                             int (*callback)(void *, int, char **, char **), void *context,
                             char **error) {
    using Exec = int (*)(sqlite3 *, const char *, int (*)(void *, int, char **, char **), void *, char **);
    static auto real = reinterpret_cast<Exec>(::dlsym(RTLD_NEXT, "sqlite3_exec"));
    const auto *trace = std::getenv("CH_A53_SQL_COST");
    const bool begin = trace && std::strcmp(statement, "BEGIN IMMEDIATE") == 0;
    if (begin) {
        costActive = true;
        costLegacy = false;
        ::clock_gettime(CLOCK_MONOTONIC, &costStarted);
    }
    const int result = real(db, statement, callback, context, error);
    if (begin && result != 0) costActive = false;
    if (costActive && (std::strcmp(statement, "COMMIT") == 0 ||
                       std::strcmp(statement, "ROLLBACK") == 0)) {
        if (costLegacy && trace) {
            timespec end{};
            ::clock_gettime(CLOCK_MONOTONIC, &end);
            const double seconds = end.tv_sec - costStarted.tv_sec +
                                   (end.tv_nsec - costStarted.tv_nsec) / 1e9;
            char record[96];
            const int length = std::snprintf(record, sizeof record, "%s %d %.9f\n", statement, result, seconds);
            int fd = ::open(trace, O_CREAT | O_APPEND | O_WRONLY | O_CLOEXEC, 0600);
            if (fd >= 0) { (void)::write(fd, record, length); ::close(fd); }
        }
        costActive = false;
    }
    return result;
}
struct sqlite3_stmt;
extern "C" int sqlite3_step(sqlite3_stmt *stmt) {
    using Step = int (*)(sqlite3_stmt *);
    using Sql = const char *(*)(sqlite3_stmt *);
    static auto real = reinterpret_cast<Step>(::dlsym(RTLD_NEXT, "sqlite3_step"));
    static auto sql = reinterpret_cast<Sql>(::dlsym(RTLD_NEXT, "sqlite3_sql"));
    if (const auto *marker = std::getenv("CH_A53_EXPIRY_MARKER")) {
        char arm[4096];
        std::snprintf(arm, sizeof arm, "%s.arm", marker);
        if (::access(arm, F_OK) == 0 &&
            std::strncmp(sql(stmt), "SELECT id,request,created FROM delta_sessions WHERE", 51) == 0) {
            int fd = ::open(marker, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
            if (fd >= 0) { (void)::write(fd, "entered", 7); ::close(fd); }
        }
    }
    if (costActive && std::strncmp(sql(stmt), "INSERT INTO legacy_versions", 27) == 0)
        costLegacy = true;
    const int result = real(stmt);
    if (result == 101 && chosen("snapshot_before_commit")) { // SQLITE_DONE
        const auto *statement = sql(stmt);
        if (statement && std::strstr(statement, "UPDATE directory_snapshots SET state='committed'"))
            stopHere();
    }
    return result;
}

// Test-only barriers for immutable baseline and resume-query reads. Armed after
// setup so startup verification remains real. A release file permits progress.
extern "C" ssize_t pread(int fd, void *bytes, size_t count, off_t offset) {
    using Fn = ssize_t (*)(int, void *, size_t, off_t);
    static auto real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "pread"));
    if (commitBarrier(fd, "pread")) { errno = EIO; return -1; }
    const auto *gate = std::getenv("CH_PLAN_TEST_GATE");
    if (directoryContains(fd, "/.delta/"))
        gate = std::getenv("CH_QUERY_TEST_GATE");
    if (gate && (directoryContains(fd, "/.versions/") || directoryContains(fd, "/.delta/"))) {
        char selected[128]{};
        int input = ::open(gate, O_RDONLY | O_CLOEXEC);
        const auto length = input < 0 ? -1 : ::read(input, selected, sizeof(selected) - 1);
        if (input >= 0) ::close(input);
        if (length > 0 && directoryContains(fd, selected)) {
            char marker[4096], release[4096], failure[4096];
            std::snprintf(marker, sizeof marker, "%s.entered", gate);
            std::snprintf(release, sizeof release, "%s.release", gate);
            std::snprintf(failure, sizeof failure, "%s.fail", gate);
            int output = ::open(marker, O_CREAT | O_APPEND | O_WRONLY | O_CLOEXEC, 0600);
            if (output >= 0) { (void)::write(output, "x", 1); ::close(output); }
            for (int i = 0; i < 1000 && ::access(release, F_OK) != 0; ++i)
                ::usleep(10000);
            if (::access(release, F_OK) != 0 || ::access(failure, F_OK) == 0) {
                errno = EIO;
                return -1;
            }
        }
    }
    return real(fd, bytes, count, offset);
}
