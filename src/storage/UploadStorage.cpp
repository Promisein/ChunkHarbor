#include "chunkharbor/storage/UploadStorage.h"
#include "chunkharbor/common/Sha256.h"
#include "chunkharbor/storage/TempFileStore.h"
#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <linux/openat2.h>
#include <mutex>
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
    protocol::CommitResult result;
};
struct UploadStorage::Impl {
    TempFileStore temporary;
    UniqueFd root, lock, files, sessions;
    std::mutex mutex;
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
        auto scan = openSafe(sessions.get(), ".", O_RDONLY | O_DIRECTORY);
        DIR *listing = ::fdopendir(scan.get());
        io(listing != nullptr);
        (void)scan.release();
        try {
            errno = 0;
            while (auto *entry = ::readdir(listing)) {
                std::string name(entry->d_name);
                if (name == "." || name == "..")
                    continue;
                io(name.size() == 32 &&
                   name.find_first_not_of("0123456789abcdef") == std::string::npos);
                auto dir = openSafe(sessions.get(), name, O_RDONLY | O_DIRECTORY);
                struct stat info{};
                io(::fstat(dir.get(), &info) == 0 && info.st_uid == ::geteuid() &&
                   (info.st_mode & 0077) == 0);
                auto meta = regularFile(dir.get(), "meta.json", O_RDONLY);
                auto data = regularFile(dir.get(), "data", O_RDONLY);
                auto receipts = regularFile(dir.get(), "receipts.log", O_RDONLY);
                auto childScan = openSafe(dir.get(), ".", O_RDONLY | O_DIRECTORY);
                DIR *children = ::fdopendir(childScan.get());
                io(children != nullptr);
                (void)childScan.release();
                bool unexpected = false;
                errno = 0;
                while (auto *child = ::readdir(children)) {
                    std::string file(child->d_name);
                    if (file != "." && file != ".." && file != "data" && file != "meta.json" &&
                        file != "receipts.log")
                        unexpected = true;
                }
                const bool scanOk = errno == 0;
                ::closedir(children);
                io(scanOk && !unexpected);
                auto b = protocol::decodeBeginUpload(readMeta(meta.get()));
                quotaGeometry(b);
                io(::fstat(data.get(), &info) == 0 && info.st_size >= 0 &&
                   static_cast<std::uint64_t>(info.st_size) <= b.size);
                std::string pending;
                char receiptBuffer[4096];
                std::set<std::uint64_t> seen;
                for (;;) {
                    const auto n = ::read(receipts.get(), receiptBuffer, sizeof receiptBuffer);
                    if (n < 0 && errno == EINTR)
                        continue;
                    io(n >= 0);
                    if (!n)
                        break;
                    pending.append(receiptBuffer, static_cast<std::size_t>(n));
                    for (;;) {
                        const auto newline = pending.find('\n');
                        if (newline == std::string::npos)
                            break;
                        io(newline <= 2048);
                        const auto ack = protocol::decodeChunkAck(pending.substr(0, newline));
                        std::string identity;
                        const char *hex = "0123456789abcdef";
                        for (auto byte : ack.sessionId) {
                            identity += hex[byte >> 4];
                            identity += hex[byte & 15];
                        }
                        io(identity == name && ack.offset < b.size &&
                           ack.offset % b.chunkSize == 0 &&
                           ack.dataLength ==
                               std::min<std::uint64_t>(b.chunkSize, b.size - ack.offset) &&
                           seen.insert(ack.offset).second);
                        pending.erase(0, newline + 1);
                    }
                    io(pending.size() <= 2048);
                }
                io(pending
                       .empty()); // Torn or publication-state receipts require M3 reconciliation.
                io(paths.insert(b.path).second);
                reserved += b.size;
                ++count;
                io(count <= 32 && reserved <= 512ULL * 1024 * 1024);
                errno = 0;
            }
            io(errno == 0);
        } catch (...) {
            ::closedir(listing);
            throw;
        }
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
