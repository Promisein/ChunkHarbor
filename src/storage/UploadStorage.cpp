#include "chunkharbor/storage/UploadStorage.h"
#include "chunkharbor/common/Sha256.h"
#include "chunkharbor/storage/TempFileStore.h"
#include "chunkharbor/storage/SqliteStore.h"
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <linux/openat2.h>
#include <mutex>
#include <map>
#include <set>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <vector>
namespace chunkharbor {
namespace {
[[noreturn]] void fail(const char *code, const char *message) {
    throw protocol::UploadFailure(code, message);
}
void io(bool ok) {
    if (!ok)
        fail(errno == ENOSPC || errno == EDQUOT ? "DISK_FULL" : "INTERNAL_ERROR",
             "storage operation failed");
}
UniqueFd openSafe(int parent, const std::string &path, int flags, mode_t mode = 0) {
    open_how how{static_cast<unsigned long long>(flags | O_CLOEXEC | O_NOFOLLOW), mode,
                 RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS | RESOLVE_NO_XDEV};
    long fd;
    do {
        fd = ::syscall(SYS_openat2, parent, path.c_str(), &how, sizeof(how));
    } while (fd < 0 && errno == EINTR);
    if (fd < 0 && (errno == ELOOP || errno == EXDEV || errno == ENOTDIR))
        fail("INVALID_PATH", "unsafe destination path");
    io(fd >= 0);
    return UniqueFd(static_cast<int>(fd));
}
void sync(int fd, bool data = false) {
    int r;
    do {
        r = data ? ::fdatasync(fd) : ::fsync(fd);
    } while (r < 0 && errno == EINTR);
    io(r == 0);
}
void append(int fd, std::string_view text) {
    while (!text.empty()) {
        auto n = ::write(fd, text.data(), text.size());
        if (n < 0 && errno == EINTR)
            continue;
        io(n > 0);
        text.remove_prefix(static_cast<std::size_t>(n));
    }
}
void record(int fd, const std::string &text) {
    append(fd, text);
    append(fd, "\n");
    sync(fd, true);
}
UniqueFd regularFile(int parent, const std::string &name, int flags, mode_t mode = 0) {
    auto fd = openSafe(parent, name, flags | O_NONBLOCK, mode);
    struct stat info{};
    io(::fstat(fd.get(), &info) == 0 && S_ISREG(info.st_mode) && info.st_uid == ::geteuid() &&
       (info.st_mode & 0077) == 0 && info.st_nlink == 1);
    return fd;
}
UniqueFd directory(int parent, const std::string &name) {
    if (::mkdirat(parent, name.c_str(), 0700) < 0)
        io(errno == EEXIST);
    auto fd = openSafe(parent, name, O_RDONLY | O_DIRECTORY);
    struct stat st{};
    io(::fstat(fd.get(), &st) == 0);
    io(st.st_uid == ::geteuid() && (st.st_mode & 0077) == 0);
    return fd;
}
std::uint64_t blocks(const protocol::BeginUpload &b) {
    return b.size / b.chunkSize + (b.size % b.chunkSize != 0);
}
void quotaGeometry(const protocol::BeginUpload &b) {
    if (b.size > 64ULL * 1024 * 1024 ||
        b.size > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) || blocks(b) > 65536)
        fail("QUOTA_EXCEEDED", "file exceeds upload limits");
}
std::string readMeta(int fd) {
    std::string out;
    char buffer[4096];
    for (;;) {
        auto n = ::read(fd, buffer, sizeof buffer);
        if (n < 0 && errno == EINTR)
            continue;
        io(n >= 0);
        if (!n)
            break;
        out.append(buffer, static_cast<std::size_t>(n));
        io(out.size() <= 16384);
    }
    return out;
}
std::string hashFd(int fd, std::uint64_t size) {
    Sha256 hash;
    char buffer[65536];
    for (std::uint64_t offset = 0; offset < size;) {
        auto n = ::pread(fd, buffer, std::min<std::uint64_t>(sizeof buffer, size - offset),
                         static_cast<off_t>(offset));
        if (n < 0 && errno == EINTR) continue;
        io(n > 0);
        hash.update(std::string_view(buffer, static_cast<std::size_t>(n)));
        offset += static_cast<std::uint64_t>(n);
    }
    return toHex(hash.finish());
}
} // namespace
struct UploadSession {
    std::mutex mutex;
    const void *owner = nullptr;
    protocol::BeginUpload begin;
    std::array<std::uint8_t, 16> id{};
    std::string name;
    UniqueFd dir, data, receipts;
    std::vector<bool> accepted;
    std::vector<Sha256Digest> hashes;
    bool poisoned = false, committed = false;
    std::string dbState = "CREATED";
    protocol::CommitResult result;
};
struct UploadStorage::Impl {
    TempFileStore temporary;
    UniqueFd root, lock, files, sessions;
    std::mutex mutex;
    std::mutex dbMutex;
    std::unique_ptr<SqliteStore> db;
    std::vector<UploadSessionPtr> retained;
    std::set<std::string> paths;
    std::uint64_t reserved = 0;
    std::size_t count = 0;
    explicit Impl(std::string_view path) : temporary(path) {
        open_how how{O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0,
                     RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS};
        std::string p(path);
        root.reset(static_cast<int>(::syscall(SYS_openat2, AT_FDCWD, p.c_str(), &how, sizeof how)));
        io(bool(root));
        lock = regularFile(root.get(), ".lock", O_CREAT | O_RDWR, 0600);
        io(::flock(lock.get(), LOCK_EX | LOCK_NB) == 0);
        files = directory(root.get(), "files");
        sessions = directory(root.get(), ".sessions");
        sync(root.get());
        auto dbFile = regularFile(root.get(), "metadata.sqlite3", O_CREAT | O_RDWR, 0600);
        dbFile.reset();
        db = std::make_unique<SqliteStore>(std::string(path) + "/metadata.sqlite3",
                                           CHUNKHARBOR_SCHEMA_V1_PATH);
        auto quarantine = directory(root.get(), ".quarantine");
        sync(root.get());
        auto cleanupSession = [&](int dirfd, const std::string &name) {
            for (const char *file : {"data", "meta.json", "receipts.log"})
                if (::unlinkat(dirfd, file, 0) < 0) io(errno == ENOENT);
            sync(dirfd);
            io(::unlinkat(sessions.get(), name.c_str(), AT_REMOVEDIR) == 0);
            sync(sessions.get());
        };
        std::map<std::string, SqliteStore::Transfer> known;
        for (auto &t : db->transfers()) known.emplace(t.sessionId, std::move(t));
        auto scan = openSafe(sessions.get(), ".", O_RDONLY | O_DIRECTORY);
        DIR *listing = ::fdopendir(scan.get());
        io(listing != nullptr);
        (void)scan.release();
        try {
            errno = 0;
            while (auto *entry = ::readdir(listing)) {
                std::string name(entry->d_name);
                if (name == "." || name == "..") continue;
                io(name.size() == 32 &&
                   name.find_first_not_of("0123456789abcdef") == std::string::npos);
                auto it = known.find(name);
                if (it == known.end()) {
                    io(::syscall(SYS_renameat2, sessions.get(), name.c_str(),
                                 quarantine.get(), name.c_str(), RENAME_NOREPLACE) == 0);
                    sync(sessions.get());
                    sync(quarantine.get());
                    errno = 0;
                    continue;
                }
                auto &t = it->second;
                auto dir = openSafe(sessions.get(), name, O_RDONLY | O_DIRECTORY);
                struct stat directoryInfo{};
                io(::fstat(dir.get(), &directoryInfo) == 0 &&
                   directoryInfo.st_uid == ::geteuid() &&
                   (directoryInfo.st_mode & 0077) == 0);
                auto childScan = openSafe(dir.get(), ".", O_RDONLY | O_DIRECTORY);
                DIR *children = ::fdopendir(childScan.get());
                io(children != nullptr);
                (void)childScan.release();
                bool unexpected = false;
                errno = 0;
                while (auto *child = ::readdir(children)) {
                    const std::string file(child->d_name);
                    if (file != "." && file != ".." && file != "data" &&
                        file != "meta.json" && file != "receipts.log")
                        unexpected = true;
                }
                const bool scanned = errno == 0;
                ::closedir(children);
                io(scanned && !unexpected);
                auto meta = regularFile(dir.get(), "meta.json", O_RDONLY);
                auto b = protocol::decodeBeginUpload(readMeta(meta.get()));
                io(b.path == t.logicalPath && b.sha256 == t.expectedSha256 &&
                   b.size == static_cast<std::uint64_t>(t.totalSize) &&
                   b.chunkSize == static_cast<std::uint32_t>(t.chunkSize) &&
                   b.mtimeNs == t.mtimeNs && t.tempPath == ".sessions/" + name + "/data");
                const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                constexpr std::int64_t retentionNs = 30LL * 24 * 60 * 60 * 1000000000;
                if ((t.state == "CREATED" || t.state == "RECEIVING") &&
                    t.updatedAtNs < now - retentionNs) {
                    db->setState(name, t.state, "EXPIRED");
                    t.state = "EXPIRED";
                }
                if (t.state == "FAILED") {
                    io(::syscall(SYS_renameat2, sessions.get(), name.c_str(),
                                 quarantine.get(), name.c_str(), RENAME_NOREPLACE) == 0);
                    sync(sessions.get()); sync(quarantine.get());
                    known.erase(it); errno = 0; continue;
                }
                if (t.state == "EXPIRED") {
                    cleanupSession(dir.get(), name);
                    known.erase(it); errno = 0; continue;
                }
                if (t.state == "COMMITTED") {
                    auto destination = target(b.path);
                    auto file = regularFile(destination.first.get(), destination.second, O_RDONLY);
                    struct stat publishedInfo{};
                    io(::fstat(file.get(), &publishedInfo) == 0 &&
                       publishedInfo.st_size == static_cast<off_t>(b.size) &&
                       hashFd(file.get(), b.size) == b.sha256);
                    cleanupSession(dir.get(), name);
                    known.erase(it); errno = 0; continue;
                }
                auto targetFile = target(b.path);
                struct stat st{};
                bool published = ::fstatat(targetFile.first.get(), targetFile.second.c_str(),
                                           &st, AT_SYMLINK_NOFOLLOW) == 0;
                if (published && t.state != "COMMITTING") fail("CONFLICT", "unpublished session target exists");
                if (published) {
                    auto final = regularFile(targetFile.first.get(), targetFile.second, O_RDONLY);
                    io(st.st_size >= 0 && static_cast<std::uint64_t>(st.st_size) == b.size &&
                       hashFd(final.get(), b.size) == b.sha256);
                    sync(targetFile.first.get());
                    db->setState(name, "COMMITTING", "COMMITTED");
                    cleanupSession(dir.get(), name);
                    known.erase(it); errno = 0; continue;
                }
                auto data = regularFile(dir.get(), "data", O_RDWR);
                auto receipts = regularFile(dir.get(), "receipts.log", O_WRONLY | O_APPEND);
                io(::fstat(data.get(), &st) == 0 && st.st_size >= 0 &&
                   static_cast<std::uint64_t>(st.st_size) <= b.size);
                auto session = std::make_shared<UploadSession>();
                session->owner = this;
                session->begin = b;
                session->name = name;
                session->dbState = t.state == "VERIFYING" ? "RECEIVING" : t.state;
                for (std::size_t j = 0; j < 16; ++j) {
                    auto digit = [](char c) -> std::uint8_t {
                        return c <= '9' ? static_cast<std::uint8_t>(c - '0')
                                        : static_cast<std::uint8_t>(c - 'a' + 10);
                    };
                    session->id[j] = static_cast<std::uint8_t>((digit(name[2*j]) << 4) | digit(name[2*j+1]));
                }
                session->accepted.resize(blocks(b));
                session->hashes.resize(blocks(b));
                for (const auto &chunk : db->chunks(name)) {
                    io(chunk.index >= 0 && static_cast<std::uint64_t>(chunk.index) < blocks(b));
                    const auto offset = static_cast<std::uint64_t>(chunk.index) * b.chunkSize;
                    const auto length = std::min<std::uint64_t>(b.chunkSize, b.size - offset);
                    std::string bytes(length, '\0');
                    std::size_t done = 0;
                    while (done < bytes.size()) {
                        auto n = ::pread(data.get(), bytes.data() + done, bytes.size() - done,
                                         static_cast<off_t>(offset + done));
                        if (n < 0 && errno == EINTR) continue;
                        if (n <= 0) break;
                        done += static_cast<std::size_t>(n);
                    }
                    if (done != bytes.size() || toHex(sha256(bytes)) != chunk.sha256) {
                        db->forgetChunk(name, chunk.index);
                        continue;
                    }
                    session->accepted[chunk.index] = true;
                    session->hashes[chunk.index] = sha256(bytes);
                }
                if (t.state == "VERIFYING") db->setState(name, "VERIFYING", "RECEIVING");
                if (t.state == "COMMITTING" &&
                    std::find(session->accepted.begin(), session->accepted.end(), false) !=
                        session->accepted.end()) {
                    db->setState(name, "COMMITTING", "RECEIVING");
                    session->dbState = "RECEIVING";
                }
                session->dir = std::move(dir);
                session->data = std::move(data);
                session->receipts = std::move(receipts);
                io(paths.insert(b.path).second);
                reserved += b.size; ++count;
                io(count <= 32 && reserved <= 512ULL * 1024 * 1024);
                retained.push_back(std::move(session));
                known.erase(it); errno = 0;
            }
            io(errno == 0);
            for (const auto &[name, transfer] : known) {
                if (transfer.state == "COMMITTED") {
                    io(transfer.totalSize >= 0);
                    auto destination = target(transfer.logicalPath);
                    auto file = regularFile(destination.first.get(), destination.second, O_RDONLY);
                    struct stat info{};
                    io(::fstat(file.get(), &info) == 0 && info.st_size == transfer.totalSize &&
                       hashFd(file.get(), static_cast<std::uint64_t>(transfer.totalSize)) ==
                           transfer.expectedSha256);
                } else if (transfer.state != "FAILED" && transfer.state != "EXPIRED") {
                    db->setState(name, transfer.state, "FAILED");
                }
            }
        } catch (...) { ::closedir(listing); throw; }
        ::closedir(listing);
    }
    std::pair<UniqueFd, std::string> target(const std::string &path) {
        auto fd = openSafe(files.get(), ".", O_RDONLY | O_DIRECTORY);
        std::size_t start = 0;
        for (;;) {
            auto end = path.find('/', start);
            if (end == std::string::npos)
                return {std::move(fd), path.substr(start)};
            auto next = directory(fd.get(), path.substr(start, end - start));
            sync(fd.get());
            fd = std::move(next);
            start = end + 1;
        }
    }
};
UploadStorage::UploadStorage(std::string_view root) : impl_(std::make_unique<Impl>(root)) {}
UploadStorage::~UploadStorage() = default;
QueriedUpload UploadStorage::query(const protocol::QuerySession &request) {
    auto &i = *impl_;
    std::string name;
    for (auto byte : request.sessionId) {
        name += "0123456789abcdef"[byte >> 4];
        name += "0123456789abcdef"[byte & 15];
    }
    UploadSessionPtr session;
    {
        std::lock_guard<std::mutex> guard(i.mutex);
        for (const auto &item : i.retained)
            if (item->name == name) { session = item; break; }
    }
    if (!session) {
        std::lock_guard<std::mutex> guard(i.dbMutex);
        for (const auto &t : i.db->transfers())
            if (t.sessionId == name && t.state == "COMMITTED")
                return {{}, {}, true,
                        {request.sessionId, t.logicalPath,
                         static_cast<std::uint64_t>(t.totalSize), t.expectedSha256}};
        fail("SESSION_NOT_FOUND", "unknown or unavailable upload session");
    }
    std::lock_guard<std::mutex> guard(session->mutex);
    if (session->poisoned) fail("SESSION_NOT_FOUND", "session requires recovery");
    protocol::SessionStatus status;
    status.sessionId = session->id;
    status.chunkSize = session->begin.chunkSize;
    status.blockCount = static_cast<std::uint32_t>(session->accepted.size());
    status.missing.resize((session->accepted.size() + 7) / 8);
    for (std::size_t index = 0; index < session->accepted.size(); ++index) {
        if (!session->accepted[index])
            status.missing[index / 8] |= static_cast<std::uint8_t>(1U << (index % 8));
    }
    return {session, std::move(status), false, {}};
}
StartedUpload UploadStorage::begin(const protocol::BeginUpload &b) {
    std::string json;
    try {
        json = protocol::encodeBeginUpload(b);
    } catch (const std::exception &) {
        fail("MALFORMED_MESSAGE", "invalid upload metadata");
    }
    quotaGeometry(b);
    auto &i = *impl_;
    std::lock_guard<std::mutex> guard(i.mutex);
    if (i.count >= 32 || b.size > 512ULL * 1024 * 1024 - i.reserved)
        fail("QUOTA_EXCEEDED", "retained upload limit reached");
    if (i.paths.count(b.path))
        fail("CONFLICT", "target has an unfinished upload");
    auto target = i.target(b.path);
    struct stat st{};
    if (::fstatat(target.first.get(), target.second.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0)
        fail("CONFLICT", "destination already exists");
    io(errno == ENOENT);
    auto s = std::make_shared<UploadSession>();
    s->owner = &i;
    s->begin = b;
    s->accepted.resize(blocks(b));
    s->hashes.resize(blocks(b));
    std::size_t got = 0;
    while (got < s->id.size()) {
        auto n = ::getrandom(s->id.data() + got, s->id.size() - got, 0);
        if (n < 0 && errno == EINTR)
            continue;
        io(n > 0);
        got += static_cast<std::size_t>(n);
    }
    const char *hex = "0123456789abcdef";
    for (auto byte : s->id) {
        s->name += hex[byte >> 4];
        s->name += hex[byte & 15];
    }
    io(::mkdirat(i.sessions.get(), s->name.c_str(), 0700) == 0);
    // Once the name exists, reserve even on failure; unknown/partial state is fail-closed on
    // restart.
    ++i.count;
    i.reserved += b.size;
    i.paths.insert(b.path);
    try {
        s->dir = openSafe(i.sessions.get(), s->name, O_RDONLY | O_DIRECTORY);
        s->data = i.temporary.create(".sessions/" + s->name);
        const auto source = "/proc/self/fd/" + std::to_string(s->data.get());
        io(::linkat(AT_FDCWD, source.c_str(), s->dir.get(), "data", AT_SYMLINK_FOLLOW) == 0);
        auto meta = openSafe(s->dir.get(), "meta.json", O_CREAT | O_EXCL | O_WRONLY, 0600);
        append(meta.get(), json);
        sync(meta.get());
        s->receipts =
            openSafe(s->dir.get(), "receipts.log", O_CREAT | O_EXCL | O_WRONLY | O_APPEND, 0600);
        sync(s->receipts.get());
        sync(s->data.get());
        sync(s->dir.get());
        sync(i.sessions.get());
        sync(i.root.get());
        {
            std::lock_guard<std::mutex> dbGuard(i.dbMutex);
            i.db->createTransfer({s->name, b.path, b.sha256, "CREATED",
                                  static_cast<std::int64_t>(b.size), b.chunkSize,
                                  b.mtimeNs, 0, ".sessions/" + s->name + "/data"});
        }
    } catch (...) {
        // No ACK exists yet. Remove only names created by this begin operation.
        if (s->dir) {
            ::unlinkat(s->dir.get(), "data", 0);
            ::unlinkat(s->dir.get(), "meta.json", 0);
            ::unlinkat(s->dir.get(), "receipts.log", 0);
        }
        if (::unlinkat(i.sessions.get(), s->name.c_str(), AT_REMOVEDIR) == 0) {
            --i.count;
            i.reserved -= b.size;
            i.paths.erase(b.path);
        }
        throw;
    }
    i.retained.push_back(s);
    protocol::UploadPlan plan;
    plan.sessionId = s->id;
    plan.chunkSize = b.chunkSize;
    if (blocks(b))
        plan.missingRanges.push_back({0, blocks(b)});
    return {s, plan};
}
protocol::ChunkAck UploadStorage::write(const UploadSessionPtr &s,
                                        const protocol::DataChunk &chunk) {
    if (!s || s->owner != impl_.get())
        fail("MALFORMED_MESSAGE", "invalid upload session");
    std::lock_guard<std::mutex> guard(s->mutex);
    if (chunk.sessionId != s->id)
        fail("SESSION_NOT_FOUND", "unknown upload session");
    if (s->poisoned || s->committed)
        fail("MALFORMED_MESSAGE", "upload state mismatch");
    try {
        protocol::validateDataChunkRange(chunk, s->begin.size, s->begin.chunkSize);
    } catch (const std::exception &) {
        fail("MALFORMED_MESSAGE", "invalid chunk range");
    }
    if (sha256(chunk.data) != chunk.sha256)
        fail("CHECKSUM_MISMATCH", "chunk digest mismatch");
    const auto index = static_cast<std::size_t>(chunk.offset / s->begin.chunkSize);
    protocol::ChunkAck ack{s->id, chunk.offset, static_cast<std::uint32_t>(chunk.data.size()),
                           chunk.sha256};
    if (s->accepted[index]) {
        if (s->hashes[index] != chunk.sha256)
            fail("CONFLICT", "accepted chunk differs");
        return ack;
    }
    try {
        std::size_t done = 0;
        while (done < chunk.data.size()) {
            auto n = ::pwrite(s->data.get(), chunk.data.data() + done, chunk.data.size() - done,
                              static_cast<off_t>(chunk.offset + done));
            if (n < 0 && errno == EINTR)
                continue;
            io(n > 0);
            done += static_cast<std::size_t>(n);
        }
        sync(s->data.get(), true);
        record(s->receipts.get(), protocol::encodeChunkAck(ack));
        {
            std::lock_guard<std::mutex> dbGuard(impl_->dbMutex);
            const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            impl_->db->confirmChunk(s->name, static_cast<std::int64_t>(index),
                                    toHex(chunk.sha256), now);
        }
        s->dbState = "RECEIVING";
        s->hashes[index] = chunk.sha256;
        s->accepted[index] = true;
    } catch (...) {
        s->poisoned = true;
        throw;
    }
    return ack;
}
protocol::CommitResult UploadStorage::commit(const UploadSessionPtr &s,
                                             const protocol::CommitUpload &request) {
    if (!s || s->owner != impl_.get())
        fail("MALFORMED_MESSAGE", "invalid upload session");
    std::lock_guard<std::mutex> guard(s->mutex);
    if (request.sessionId != s->id)
        fail("SESSION_NOT_FOUND", "unknown upload session");
    if (s->poisoned)
        fail("MALFORMED_MESSAGE", "upload state mismatch");
    if (s->committed)
        return s->result;
    for (bool received : s->accepted)
        if (!received)
            fail("INCOMPLETE_UPLOAD", "missing chunks");
    Sha256 hash;
    char buffer[65536];
    std::uint64_t offset = 0;
    while (offset < s->begin.size) {
        auto n = ::pread(s->data.get(), buffer,
                         std::min<std::uint64_t>(sizeof buffer, s->begin.size - offset),
                         static_cast<off_t>(offset));
        if (n < 0 && errno == EINTR)
            continue;
        io(n > 0);
        hash.update(std::string_view(buffer, static_cast<std::size_t>(n)));
        offset += static_cast<std::uint64_t>(n);
    }
    if (toHex(hash.finish()) != s->begin.sha256)
        fail("CHECKSUM_MISMATCH", "file digest mismatch");
    try {
        sync(s->data.get());
        if (s->dbState != "COMMITTING") {
            std::lock_guard<std::mutex> dbGuard(impl_->dbMutex);
            impl_->db->setState(s->name, s->dbState, "VERIFYING");
            impl_->db->setState(s->name, "VERIFYING", "COMMITTING");
            s->dbState = "COMMITTING";
        }
        record(s->receipts.get(), "COMMITTING");
        auto target = impl_->target(s->begin.path);
        if (::syscall(SYS_renameat2, s->dir.get(), "data", target.first.get(),
                      target.second.c_str(), RENAME_NOREPLACE) < 0) {
            if (errno == EEXIST)
                fail("CONFLICT", "destination already exists");
            io(false);
        }
        sync(target.first.get());
        sync(s->dir.get());
        s->result = {s->id, s->begin.path, s->begin.size, s->begin.sha256};
        record(s->receipts.get(), "COMMITTED " + protocol::encodeCommitResult(s->result));
        {
            std::lock_guard<std::mutex> dbGuard(impl_->dbMutex);
            impl_->db->setState(s->name, "COMMITTING", "COMMITTED");
            s->dbState = "COMMITTED";
        }
        s->committed = true;
    } catch (...) {
        s->poisoned = true;
        throw;
    }
    // Successful own sessions only. Cleanup failures retain conservative quota/path pins.
    if (::unlinkat(s->dir.get(), "meta.json", 0) == 0 &&
        ::unlinkat(s->dir.get(), "receipts.log", 0) == 0 &&
        ::unlinkat(impl_->sessions.get(), s->name.c_str(), AT_REMOVEDIR) == 0) {
        sync(impl_->sessions.get());
        std::lock_guard<std::mutex> lock(impl_->mutex);
        --impl_->count;
        impl_->reserved -= s->begin.size;
        impl_->paths.erase(s->begin.path);
    }
    s->data.reset();
    s->receipts.reset();
    s->dir.reset();
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto &retained = impl_->retained;
        retained.erase(std::remove(retained.begin(), retained.end(), s), retained.end());
    }
    return s->result;
}
} // namespace chunkharbor
