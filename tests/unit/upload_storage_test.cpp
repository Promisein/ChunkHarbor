#include "chunkharbor/common/Sha256.h"
#include "chunkharbor/storage/UploadStorage.h"
#include <atomic>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
static int writeFault = 0, syncFault = 0, receiptWriteFault = 0;
static bool directorySyncFault = false;
extern "C" ssize_t __real_pwrite(int, const void *, size_t, off_t);
extern "C" int __real_fdatasync(int);
extern "C" ssize_t __real_write(int, const void *, size_t);
extern "C" int __real_fsync(int);
extern "C" ssize_t __wrap_write(int fd, const void *data, size_t count) {
    if (receiptWriteFault == 1) {
        receiptWriteFault = 2;
        errno = EINTR;
        return -1;
    }
    if (receiptWriteFault == 2) {
        receiptWriteFault = 0;
        return __real_write(fd, data, 1);
    }
    if (receiptWriteFault == 3) {
        receiptWriteFault = 0;
        errno = ENOSPC;
        return -1;
    }
    return __real_write(fd, data, count);
}
extern "C" int __wrap_fsync(int fd) {
    struct stat status{};
    if (directorySyncFault && ::fstat(fd, &status) == 0 && S_ISDIR(status.st_mode)) {
        directorySyncFault = false;
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}
extern "C" ssize_t __wrap_pwrite(int fd, const void *data, size_t count, off_t offset) {
    if (writeFault == 1) {
        writeFault = 2;
        errno = EINTR;
        return -1;
    }
    if (writeFault == 2) {
        writeFault = 0;
        return __real_pwrite(fd, data, 1, offset);
    }
    if (writeFault == 3) {
        writeFault = 0;
        return 0;
    }
    return __real_pwrite(fd, data, count, offset);
}
extern "C" int __wrap_fdatasync(int fd) {
    if (syncFault && --syncFault == 0) {
        errno = EIO;
        return -1;
    }
    return __real_fdatasync(fd);
}
using namespace chunkharbor;
namespace fs = std::filesystem;
void check(bool b) {
    if (!b)
        throw std::runtime_error("storage assertion");
}
template <class F> void rejects(F f) {
    try {
        f();
    } catch (const std::exception &) {
        return;
    }
    throw std::runtime_error("expected rejection");
}
void fifoStartupRegression(const fs::path &root, const char *name) {
    fs::create_directory(root);
    ::chmod(root.c_str(), 0700);
    {
        UploadStorage initial(root.string());
        protocol::BeginUpload empty;
        empty.path = "empty";
        empty.sha256 = toHex(sha256(""));
        initial.begin(empty);
    }
    const auto session = fs::directory_iterator(root / ".sessions")->path();
    fs::remove(session / name);
    check(::mkfifo((session / name).c_str(), 0600) == 0);
    const auto pid = ::fork();
    check(pid >= 0);
    if (pid == 0) {
        ::alarm(2);
        try {
            UploadStorage broken(root.string());
            ::_exit(1);
        } catch (const std::exception &) {
            ::_exit(0);
        }
    }
    int status = 0;
    check(::waitpid(pid, &status, 0) == pid);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
int main() {
    try {
        char pattern[] = "/tmp/chunkharbor-upload-XXXXXX";
        auto raw = ::mkdtemp(pattern);
        if (!raw)
            return 1;
        fs::path root(raw);
        struct Cleanup {
            fs::path p;
            ~Cleanup() { fs::remove_all(p); }
        } cleanup{root};
        UploadStorage store(root.string());
        protocol::BeginUpload b;
        b.path = "dir/binary";
        b.chunkSize = 4;
        b.size = 5;
        const std::string content("a\0bcd", 5);
        b.sha256 = toHex(sha256(content));
        auto started = store.begin(b);
        rejects([&] { store.begin(b); });
        protocol::DataChunk d;
        d.sessionId = started.plan.sessionId;
        d.data = content.substr(0, 4);
        d.sha256 = sha256(d.data);
        writeFault = 1;
        receiptWriteFault = 1;
        store.write(started.session, d);
        store.write(started.session, d);
        check(writeFault == 0 && receiptWriteFault == 0);
        auto bad = d;
        bad.data = "xxxx";
        bad.sha256 = sha256(bad.data);
        rejects([&] { store.write(started.session, bad); });
        protocol::CommitUpload c;
        c.sessionId = d.sessionId;
        rejects([&] { store.commit(started.session, c); });
        d.offset = 4;
        d.data = content.substr(4);
        d.sha256 = sha256(d.data);
        store.write(started.session, d);
        auto result = store.commit(started.session, c);
        check(result.sha256 == b.sha256);
        check(store.commit(started.session, c).path == b.path);
        check(toHex(sha256File(root / "files" / b.path)) == b.sha256);
        rejects([&] { store.begin(b); });
        b.path = "empty";
        b.size = 0;
        b.sha256 = toHex(sha256(""));
        auto empty = store.begin(b);
        c.sessionId = empty.plan.sessionId;
        store.commit(empty.session, c);
        b.path = "../escape";
        rejects([&] { store.begin(b); });
        fs::create_directory_symlink(root, root / "files" / "link");
        b.path = "link/escape";
        rejects([&] { store.begin(b); });
        b.path = "too-large";
        b.size = 64ULL * 1024 * 1024 + 1;
        rejects([&] { store.begin(b); });
        b.path = "fault";
        b.size = 1;
        b.sha256 = toHex(sha256("z"));
        auto fault = store.begin(b);
        d.sessionId = fault.plan.sessionId;
        d.offset = 0;
        d.data = "z";
        d.sha256 = sha256("z");
        syncFault = 1;
        rejects([&] { store.write(fault.session, d); });
        rejects([&] { store.write(fault.session, d); });
        b.path = "receipt-fault";
        auto receipt = store.begin(b);
        d.sessionId = receipt.plan.sessionId;
        syncFault = 2;
        rejects([&] { store.write(receipt.session, d); });
        rejects([&] { store.write(receipt.session, d); });
        b.path = "receipt-write-full";
        auto full = store.begin(b);
        d.sessionId = full.plan.sessionId;
        receiptWriteFault = 3;
        try {
            store.write(full.session, d);
            throw std::runtime_error("ACK after full receipt disk");
        } catch (const protocol::UploadFailure &e) {
            check(e.code == "DISK_FULL");
        }
        rejects([&] { store.write(full.session, d); });
        b.path = "no-overwrite-race";
        auto race = store.begin(b);
        d.sessionId = race.plan.sessionId;
        store.write(race.session, d);
        std::ofstream(root / "files" / b.path) << "sentinel";
        c.sessionId = d.sessionId;
        rejects([&] { store.commit(race.session, c); });
        check(toHex(sha256File(root / "files" / b.path)) == toHex(sha256("sentinel")));
        b.path = "zero-write";
        auto zero = store.begin(b);
        d.sessionId = zero.plan.sessionId;
        writeFault = 3;
        rejects([&] { store.write(zero.session, d); });
        b.path = "wrong-whole";
        b.sha256 = toHex(sha256("x"));
        auto wrong = store.begin(b);
        d.sessionId = wrong.plan.sessionId;
        store.write(wrong.session, d);
        c.sessionId = d.sessionId;
        rejects([&] { store.commit(wrong.session, c); });
        b.sha256 = toHex(sha256("z"));
        b.path = "wrong-chunk";
        auto chunk = store.begin(b);
        d.sessionId = chunk.plan.sessionId;
        d.sha256 = sha256("bad");
        rejects([&] { store.write(chunk.session, d); });
        d.sha256 = sha256("z");
        d.offset = 1;
        rejects([&] { store.write(chunk.session, d); });
        d.offset = 0;
        d.sessionId = {};
        rejects([&] { store.write(chunk.session, d); });
        std::atomic<int> completed{0};
        std::exception_ptr e1, e2;
        auto upload = [&](const char *name, std::exception_ptr &error) {
            try {
                auto value = b;
                value.path = name;
                auto s = store.begin(value);
                protocol::DataChunk part;
                part.sessionId = s.plan.sessionId;
                part.data = "z";
                part.sha256 = sha256("z");
                store.write(s.session, part);
                protocol::CommitUpload end;
                end.sessionId = part.sessionId;
                store.commit(s.session, end);
                ++completed;
            } catch (...) {
                error = std::current_exception();
            }
        };
        std::thread t1(upload, "parallel1", std::ref(e1)), t2(upload, "parallel2", std::ref(e2));
        t1.join();
        t2.join();
        if (e1)
            std::rethrow_exception(e1);
        if (e2)
            std::rethrow_exception(e2);
        check(completed == 2);
        rejects([&] { UploadStorage second(root.string()); });
        auto retainedRoot = root / "retained-root";
        fs::create_directory(retainedRoot);
        ::chmod(retainedRoot.c_str(), 0700);
        UploadSessionPtr survivor;
        {
            UploadStorage retained(retainedRoot.string());
            auto value = b;
            value.size = 0;
            value.sha256 = toHex(sha256(""));
            for (int n = 0; n < 32; ++n) {
                value.path = "retained-" + std::to_string(n);
                auto s = retained.begin(value);
                survivor = s.session;
            }
            value.path = "over-quota";
            rejects([&] { retained.begin(value); });
        }
        {
            UploadStorage restarted(retainedRoot.string());
            auto value = b;
            value.path = "still-full";
            rejects([&] { restarted.begin(value); });
        }
        check(std::distance(fs::directory_iterator(retainedRoot / ".sessions"),
                            fs::directory_iterator{}) == 32);
        fifoStartupRegression(root / "fifo-meta", "meta.json");
        fifoStartupRegression(root / "fifo-data", "data");
        fifoStartupRegression(root / "fifo-receipts", "receipts.log");
        const auto publishRoot = root / "publish-fault-root";
        fs::create_directory(publishRoot);
        ::chmod(publishRoot.c_str(), 0700);
        {
            UploadStorage publishing(publishRoot.string());
            auto value = b;
            value.path = "published-but-unconfirmed";
            auto active = publishing.begin(value);
            protocol::DataChunk part{active.plan.sessionId, 0, sha256("z"), "z"};
            publishing.write(active.session, part);
            directorySyncFault = true;
            rejects([&] { publishing.commit(active.session, {active.plan.sessionId}); });
            check(!directorySyncFault);
            check(toHex(sha256File(publishRoot / "files" / value.path)) == value.sha256);
        }
        rejects([&] { UploadStorage recoveryNotImplemented(publishRoot.string()); });
        auto ackRoot = root / "ack-root";
        fs::create_directory(ackRoot);
        ::chmod(ackRoot.c_str(), 0700);
        {
            UploadStorage ackStore(ackRoot.string());
            auto value = b;
            value.path = "acked";
            auto s = ackStore.begin(value);
            protocol::DataChunk part;
            part.sessionId = s.plan.sessionId;
            part.data = "z";
            part.sha256 = sha256("z");
            ackStore.write(s.session, part);
        }
        {
            UploadStorage restarted(ackRoot.string());
            auto value = b;
            value.path = "acked";
            rejects([&] { restarted.begin(value); });
        }
        auto sessionDir = fs::directory_iterator(ackRoot / ".sessions")->path();
        check(fs::file_size(sessionDir / "data") == 1);
        std::ofstream(sessionDir / "receipts.log", std::ios::app) << "torn";
        rejects([&] { UploadStorage broken(ackRoot.string()); });
        std::cout << "upload storage passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
