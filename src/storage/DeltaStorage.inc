namespace {
struct DStmt {
    sqlite3 *db;
    sqlite3_stmt *p = nullptr;
    DStmt(sqlite3 *d, const char *q) : db(d) {
        if (sqlite3_prepare_v2(d, q, -1, &p, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(d));
    }
    ~DStmt() { sqlite3_finalize(p); }
    DStmt &text(int n, const std::string &v) {
        sqlite3_bind_text(p, n, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
        return *this;
    }
    DStmt &num(int n, std::int64_t v) {
        sqlite3_bind_int64(p, n, v);
        return *this;
    }
    bool row() {
        int r = sqlite3_step(p);
        if (r != SQLITE_ROW && r != SQLITE_DONE)
            throw std::runtime_error(sqlite3_errmsg(db));
        return r == SQLITE_ROW;
    }
    std::string str(int n) {
        auto v = sqlite3_column_text(p, n);
        return v ? reinterpret_cast<const char *>(v) : "";
    }
    std::int64_t number(int n) { return sqlite3_column_int64(p, n); }
};
void dsql(sqlite3 *db, const char *q) {
    if (sqlite3_exec(db, q, nullptr, nullptr, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db));
}
std::string did(const protocol::SessionId &id) {
    std::string s;
    for (auto b : id) {
        s += "0123456789abcdef"[b >> 4];
        s += "0123456789abcdef"[b & 15];
    }
    return s;
}
protocol::SessionId dip(const std::string &s) {
    protocol::SessionId id{};
    if (s.size() != 32)
        fail("INTERNAL_ERROR", "invalid persisted ID");
    auto n = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    for (int j = 0; j < 16; ++j)
        id[j] = static_cast<std::uint8_t>((n(s[2 * j]) << 4) | n(s[2 * j + 1]));
    return id;
}
protocol::SessionId randomId() {
    protocol::SessionId r{};
    std::size_t pos = 0;
    while (pos < r.size()) {
        auto n = ::getrandom(r.data() + pos, r.size() - pos, 0);
        if (n < 0 && errno == EINTR)
            continue;
        io(n > 0);
        pos += n;
    }
    return r;
}
void copyFd(int source, int dest, std::uint64_t size) {
    char b[65536];
    for (std::uint64_t off = 0; off < size;) {
        auto n = ::pread(source, b, std::min<std::uint64_t>(sizeof b, size - off), off);
        if (n < 0 && errno == EINTR)
            continue;
        io(n > 0);
        std::size_t done = 0;
        while (done < static_cast<std::size_t>(n)) {
            auto w = ::pwrite(dest, b + done, n - done, off + done);
            if (w < 0 && errno == EINTR)
                continue;
            io(w > 0);
            done += w;
        }
        off += n;
    }
    io(::ftruncate(dest, size) == 0);
    sync(dest);
}
} // namespace
struct DeltaStorage::Impl {
    UploadStorage::Impl &u;
    sqlite3 *db;
    UniqueFd versions, staging;
    // Protected by u.mutex/u.dbMutex, shared by materialization and revalidation.
    std::set<std::string> sessionIo;
    std::map<std::string, std::size_t> activePins;
    // Legacy objects are pinned by committed transfer rows, not delta session leases.
    // Only this transient index-owner marker is released by the operation's RAII guard.
    std::set<std::string> legacyIndexing;
    struct LegacyIndexLease {
        Impl &impl;
        std::string id;
        bool active = false;
        ~LegacyIndexLease() {
            if (active) {
                std::scoped_lock guard(impl.u.mutex, impl.u.dbMutex);
                impl.legacyIndexing.erase(id);
            }
        }
    };
    struct SessionIoLease {
        Impl &impl;
        std::string id;
        bool active = false;
        bool pinned = false;
        void pin() {
            ++impl.activePins[id];
            pinned = true;
        }
        ~SessionIoLease() {
            if (active || pinned) {
                std::scoped_lock guard(impl.u.mutex, impl.u.dbMutex);
                if (active) impl.sessionIo.erase(id);
                if (pinned) {
                    auto entry = impl.activePins.find(id);
                    if (--entry->second == 0) impl.activePins.erase(entry);
                }
            }
        }
    };
    struct S {
        std::string id, base, version;
        protocol::BeginDelta b;
        protocol::DeltaPhase phase;
        std::uint32_t page;
    };
#include "PreparationJournal.inc"
    explicit Impl(UploadStorage::Impl &store) : u(store), db(store.db->nativeHandle()) {
        versions = directory(u.root.get(), ".versions");
        staging = directory(u.root.get(), ".delta");
        preparationDir = directory(u.root.get(), ".preparations");
        sync(u.root.get());
        readPreparations();
        recoverPreparations();
        expire();
        legacy();
        isolateObjects();
        DStmt q(db, "SELECT id FROM delta_sessions WHERE phase=4");
        std::vector<std::string> pending;
        while (q.row())
            pending.push_back(q.str(0));
        for (auto &id : pending) {
            try { publish(load(id)); }
            catch (const std::exception &error) {
                throw std::runtime_error("COMMITTING recovery failed for session " + id + ": " + error.what());
            }
        }
        DStmt completed(db, "SELECT id FROM delta_sessions WHERE phase=5");
        std::vector<std::string> done;
        while (completed.row()) done.push_back(completed.str(0));
        for (const auto &id : done) {
            try { cleanupCommitted(id, false); }
            catch (const std::exception &error) {
                throw std::runtime_error("committed cleanup failed for session " + id + ": " + error.what());
            }
        }
        DStmt latest(db, "SELECT v.path,v.size,v.sha256 FROM current_files c JOIN versions v ON "
                         "v.id=c.version_id");
        while (latest.row()) {
            auto target = u.target(latest.str(0));
            auto fd = regularFile(target.first.get(), target.second, O_RDONLY);
            struct stat st{};
            io(::fstat(fd.get(), &st) == 0 && st.st_size == latest.number(1) &&
               hashFd(fd.get(), latest.number(1)) == latest.str(2));
        }
        DStmt verify(db, "SELECT id FROM delta_sessions WHERE phase IN (2,3)");
        while (verify.row()) {
            auto s = load(verify.str(0));
            auto fd = data(s);
            DStmt chunks(db, "SELECT idx,sha FROM manifest_chunks WHERE session=? AND accepted=1");
            chunks.text(1, s.id);
            while (chunks.row()) {
                auto off = chunks.number(0) * s.b.metadata.chunkSize;
                std::string bytes(
                    std::min<std::uint64_t>(s.b.metadata.chunkSize, s.b.metadata.size - off), '\0');
                std::size_t pos = 0;
                while (pos < bytes.size()) {
                    auto n = ::pread(fd.get(), bytes.data() + pos, bytes.size() - pos, off + pos);
                    if (n < 0 && errno == EINTR)
                        continue;
                    if (n <= 0)
                        break;
                    pos += n;
                }
                if (pos != bytes.size() || toHex(sha256(bytes)) != chunks.str(1)) {
                    DStmt clear(db,
                                "UPDATE manifest_chunks SET accepted=0 WHERE session=? AND idx=?");
                    clear.text(1, s.id).num(2, chunks.number(0)).row();
                }
            }
        }
        DStmt active(db, "SELECT id FROM delta_sessions WHERE phase IN (0,1,2,3,4)");
        while (active.row()) {
            auto s = load(active.str(0));
            if (s.b.mode == protocol::DeltaMode::Upload)
                u.paths.insert(s.b.metadata.path);
        }
    }
    std::uint64_t previewBytes() {
        DStmt q(db, "SELECT COALESCE((SELECT SUM(length(request)+COALESCE(length(result),0)+256) "
                    "FROM delta_sessions WHERE mode=1),0)+COALESCE((SELECT SUM(length(p.body)+128) "
                    "FROM manifest_pages p JOIN delta_sessions s ON s.id=p.session WHERE "
                    "s.mode=1),0)+128*(SELECT count(*) FROM manifest_chunks c JOIN delta_sessions "
                    "s ON s.id=c.session WHERE s.mode=1)+64*(SELECT count(*) FROM "
                    "delta_plan_ranges r JOIN delta_sessions s ON s.id=r.session WHERE s.mode=1)");
        io(q.row());
        return q.number(0);
    }
    void isolateObjects() {
        auto quarantine = directory(u.root.get(), ".quarantine");
        auto fd = openSafe(versions.get(), ".", O_RDONLY | O_DIRECTORY);
        DIR *listing = ::fdopendir(fd.get());
        io(listing != nullptr);
        fd.release();
        try {
            errno = 0;
            while (auto *entry = ::readdir(listing)) {
                std::string name(entry->d_name);
                if (name == "." || name == "..")
                    continue;
                io(name.size() == 32 &&
                   name.find_first_not_of("0123456789abcdef") == std::string::npos);
                DStmt q(db, "SELECT 1 FROM versions WHERE id=?");
                q.text(1, name);
                if (!q.row()) {
                    auto target = "version-" + name;
                    io(::syscall(SYS_renameat2, versions.get(), name.c_str(), quarantine.get(),
                                 target.c_str(), RENAME_NOREPLACE) == 0);
                    sync(versions.get());
                    sync(quarantine.get());
                }
                errno = 0;
            }
            io(errno == 0);
        } catch (...) {
            ::closedir(listing);
            throw;
        }
        ::closedir(listing);
    }
    void removeStaging(const std::string &id) {
        for (const auto &name : {id, ".publish-" + id}) {
            if (::unlinkat(staging.get(), name.c_str(), 0) < 0)
                io(errno == ENOENT);
        }
        sync(staging.get());
    }
    void discardManifest(const std::string &id) {
        for (const char *table : {"manifest_pages", "manifest_chunks", "delta_plan_ranges"}) {
            const auto sql = std::string("DELETE FROM ") + table + " WHERE session=?";
            DStmt clean(db, sql.c_str());
            clean.text(1, id).row();
        }
    }
    void cleanupCommitted(const std::string &id, bool compact = true) {
        {
            std::scoped_lock guard(u.mutex, u.dbMutex);
            io(load(id).phase == protocol::DeltaPhase::Committed);
            auto owned = preparations.find(id);
            if (owned != preparations.end()) {
                io(preparationReferenced(owned->second.version));
                forgetPreparation(id);
            }
        }
        removeStaging(id);
        // Startup preserves historical database rows from older schema fixtures.
        if (!compact) return;
        std::scoped_lock guard(u.mutex, u.dbMutex);
        dsql(db, "BEGIN IMMEDIATE");
        try {
            discardManifest(id);
            dsql(db, "COMMIT");
        } catch (...) {
            dsql(db, "ROLLBACK");
            throw;
        }
    }
    void expire(std::size_t budget = std::numeric_limits<std::size_t>::max()) {
        std::vector<std::pair<std::string, std::string>> expired;
        auto now = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
        {
            // Reclaim expired uploads before previews so a preview backlog cannot
            // retain upload quota/path reservations. Finalize the scan before writes.
            DStmt q(db, "SELECT id,request,created FROM delta_sessions WHERE (phase<4 OR phase=7) "
                        "AND ((mode=0 AND created<?1) OR (mode=1 AND created<?2)) "
                        "ORDER BY mode,created,id");
            q.num(1, now - 2592000).num(2, now - 86400);
            while (q.row()) {
                auto b = protocol::decodeBeginDelta(q.str(1));
                if (!activePins.count(q.str(0)) && now - q.number(2) > (b.mode == protocol::DeltaMode::Preview ? 86400 : 2592000)) {
                    expired.push_back(
                        {q.str(0), b.mode == protocol::DeltaMode::Upload ? b.metadata.path : ""});
                    if (expired.size() == budget) break;
                }
            }
        }
        for (auto &item : expired) {
            DStmt up(db, "UPDATE delta_sessions SET phase=7 WHERE id=?");
            up.text(1, item.first).row();
            reclaimPreparation(item.first);
            removeStaging(item.first);
            dsql(db, "BEGIN IMMEDIATE");
            try {
                discardManifest(item.first);
                DStmt clean(db, "DELETE FROM delta_sessions WHERE id=?");
                clean.text(1, item.first).row();
                dsql(db, "COMMIT");
            } catch (...) {
                dsql(db, "ROLLBACK");
                throw;
            }
            if (!item.second.empty()) {
                DStmt active(db, "SELECT 1 FROM delta_sessions WHERE logical_path=? AND mode=0 AND phase<5");
                active.text(1, item.second);
                const bool legacyActive = std::any_of(u.retained.begin(), u.retained.end(),
                    [&](const auto &session) { return session->begin.path == item.second; });
                if (!active.row() && !legacyActive) u.paths.erase(item.second);
            }
        }
        if (!expired.empty())
            sync(staging.get());
    }
    bool legacyPending(const std::string &path) {
        DStmt q(db, "SELECT 1 FROM transfers t WHERE state='COMMITTED' AND logical_path=? "
                    "AND NOT EXISTS(SELECT 1 FROM legacy_versions l WHERE l.session_id=t.session_id)");
        q.text(1, path);
        return q.row();
    }
    // Owns its locks. Online calls index only the requested path; startup indexes all.
    void legacy(const std::string &path = "") {
        std::vector<SqliteStore::Transfer> candidates;
        {
            std::scoped_lock guard(u.mutex, u.dbMutex);
            DStmt q(db, "SELECT session_id,logical_path,total_size,chunk_size,expected_sha256 "
                        "FROM transfers t WHERE state='COMMITTED' AND (?1='' OR logical_path=?1) "
                        "AND NOT EXISTS(SELECT 1 FROM legacy_versions l WHERE l.session_id=t.session_id)");
            q.text(1, path);
            while (q.row()) {
                SqliteStore::Transfer t;
                t.sessionId = q.str(0);
                t.logicalPath = q.str(1);
                t.totalSize = q.number(2);
                t.chunkSize = q.number(3);
                t.expectedSha256 = q.str(4);
                candidates.push_back(std::move(t));
            }
        }
        for (const auto &t : candidates) {
            LegacyIndexLease lease{*this, t.sessionId};
            UniqueFd src;
            struct stat identity{};
            std::string base;
            {
                std::scoped_lock guard(u.mutex, u.dbMutex);
                DStmt found(db, "SELECT version_id FROM legacy_versions WHERE session_id=?");
                found.text(1, t.sessionId);
                if (found.row()) continue;
                if (legacyIndexing.count(t.sessionId))
                    fail("BUSY", "legacy index is being prepared");
                legacyIndexing.insert(t.sessionId);
                lease.active = true;
                // UploadStorage creates and syncs this object before COMMITTED, including
                // recovery of old roots. Never read mutable files/<path> or staging here.
                src = objectFile(t.sessionId);
                io(::fstat(src.get(), &identity) == 0 && identity.st_size == t.totalSize);
                base = current(t.logicalPath);
            }
            std::vector<std::pair<std::string, std::uint32_t>> chunks;
            Sha256 whole;
            for (std::uint64_t off = 0; off < static_cast<std::uint64_t>(t.totalSize);
                 off += t.chunkSize) {
                std::string bytes(std::min<std::uint64_t>(t.chunkSize, t.totalSize - off), '\0');
                std::size_t pos = 0;
                while (pos < bytes.size()) {
                    auto n = ::pread(src.get(), bytes.data() + pos, bytes.size() - pos, off + pos);
                    if (n < 0 && errno == EINTR) continue;
                    io(n > 0);
                    pos += n;
                }
                whole.update(bytes);
                chunks.emplace_back(toHex(sha256(bytes)), static_cast<std::uint32_t>(bytes.size()));
            }
            io(toHex(whole.finish()) == t.expectedSha256);
            std::scoped_lock guard(u.mutex, u.dbMutex);
            // FD pins identity across rename, but content stability also requires these
            // stats and the full digest. The original namespace must still name that FD.
            struct stat now{}, named{};
            io(::fstat(src.get(), &now) == 0);
            UniqueFd namedFile;
            try { namedFile = objectFile(t.sessionId); }
            catch (const protocol::UploadFailure &) {
                fail("CONFLICT", "legacy object namespace changed during indexing");
            }
            io(::fstat(namedFile.get(), &named) == 0);
            const auto unchanged = [&](const struct stat &a) {
                return a.st_dev == identity.st_dev && a.st_ino == identity.st_ino &&
                       a.st_size == identity.st_size && a.st_nlink == 1 &&
                       a.st_mtim.tv_sec == identity.st_mtim.tv_sec &&
                       a.st_mtim.tv_nsec == identity.st_mtim.tv_nsec &&
                       a.st_ctim.tv_sec == identity.st_ctim.tv_sec &&
                       a.st_ctim.tv_nsec == identity.st_ctim.tv_nsec;
            };
            if (!unchanged(now) || !unchanged(named))
                fail("CONFLICT", "legacy object changed during indexing");
            DStmt transfer(db, "SELECT logical_path,total_size,chunk_size,expected_sha256,state "
                               "FROM transfers WHERE session_id=?");
            transfer.text(1, t.sessionId);
            if (!transfer.row() || transfer.str(0) != t.logicalPath ||
                transfer.number(1) != t.totalSize || transfer.number(2) != t.chunkSize ||
                transfer.str(3) != t.expectedSha256 || transfer.str(4) != "COMMITTED" ||
                current(t.logicalPath) != base)
                fail("CONFLICT", "legacy baseline changed during indexing");
            dsql(db, "BEGIN IMMEDIATE");
            try {
                DStmt v(db, "INSERT OR IGNORE INTO versions VALUES(?,?,?,?,?)");
                v.text(1, t.sessionId)
                    .text(2, t.logicalPath)
                    .num(3, t.totalSize)
                    .num(4, t.chunkSize)
                    .text(5, t.expectedSha256)
                    .row();
                for (std::size_t index = 0; index < chunks.size(); ++index) {
                    DStmt chunk(db, "INSERT INTO version_chunks VALUES(?,?,?,?)");
                    chunk.text(1, t.sessionId)
                        .num(2, index)
                        .text(3, chunks[index].first)
                        .num(4, chunks[index].second)
                        .row();
                }
                DStmt m(db, "INSERT INTO legacy_versions VALUES(?,?)");
                m.text(1, t.sessionId).text(2, t.sessionId).row();
                DStmt c(db, "INSERT OR IGNORE INTO current_files VALUES(?,?)");
                c.text(1, t.logicalPath).text(2, t.sessionId).row();
                dsql(db, "COMMIT");
            } catch (...) {
                dsql(db, "ROLLBACK");
                throw;
            }
        }
    }
    void object(const std::string &id, int fd, std::uint64_t size) {
        auto d = directory(versions.get(), id);
        struct stat st{};
        if (::fstatat(d.get(), "data", &st, AT_SYMLINK_NOFOLLOW) == 0) {
            auto f = regularFile(d.get(), "data", O_RDONLY);
            io(st.st_size == static_cast<off_t>(size) && hashFd(f.get(), size) == hashFd(fd, size));
            return;
        }
        io(errno == ENOENT);
        auto out = regularFile(d.get(), "pending", O_CREAT | O_TRUNC | O_RDWR, 0600);
        copyFd(fd, out.get(), size);
        io(::renameat(d.get(), "pending", d.get(), "data") == 0);
        sync(d.get());
        sync(versions.get());
    }
    UniqueFd objectFile(const std::string &id) {
        return regularFile(versions.get(), id + "/data", O_RDONLY);
    }
    std::string current(const std::string &path) {
        DStmt q(db, "SELECT version_id FROM current_files WHERE path=?");
        q.text(1, path);
        return q.row() ? q.str(0) : "";
    }
    S load(const std::string &id) {
        DStmt q(db, "SELECT request,base,phase,next_page,version_id,created FROM delta_sessions "
                    "WHERE id=?");
        q.text(1, id);
        if (!q.row())
            fail("SESSION_NOT_FOUND", "unknown delta session");
        S s{id,
            q.str(1),
            q.str(4),
            protocol::decodeBeginDelta(q.str(0)),
            static_cast<protocol::DeltaPhase>(q.number(2)),
            static_cast<std::uint32_t>(q.number(3))};
        auto now = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
        if (!activePins.count(id) && s.phase != protocol::DeltaPhase::Expired &&
            s.phase != protocol::DeltaPhase::Committed &&
            s.phase != protocol::DeltaPhase::Committing &&
            now - q.number(5) > (s.b.mode == protocol::DeltaMode::Preview ? 86400 : 2592000)) {
            phase(s, protocol::DeltaPhase::Expired);
        }
        if (s.phase == protocol::DeltaPhase::Expired)
            fail("SESSION_NOT_FOUND", "expired delta session");
        return s;
    }
    void phase(S &s, protocol::DeltaPhase p) {
        DStmt q(db, "UPDATE delta_sessions SET phase=? WHERE id=?");
        q.num(1, static_cast<int>(p)).text(2, s.id).row();
        s.phase = p;
    }
    std::optional<protocol::VersionId> base(const S &s) {
        return s.base.empty() ? std::optional<protocol::VersionId>{} : dip(s.base);
    }
    std::uint32_t count(const S &s) { return static_cast<std::uint32_t>(blocks(s.b.metadata)); }
    UniqueFd data(const S &s, int flags = O_RDWR) {
        return regularFile(staging.get(), s.id, flags);
    }
    struct BaseSnapshot {
        std::pair<UniqueFd, std::string> target;
        UniqueFd file;
        struct stat identity{};
        std::uint64_t size = 0;
        std::string sha;
    };
    // Called with both shared locks. File contents are verified by the caller.
    BaseSnapshot checkBase(const S &s, bool recovery = false) {
        const auto version = current(s.b.metadata.path);
        if (version != s.base && (!recovery || version != s.version))
            fail("CONFLICT", "baseline changed");
        BaseSnapshot check;
        check.target = u.target(s.b.metadata.path);
        struct stat info{};
        if (::fstatat(check.target.first.get(), check.target.second.c_str(), &info,
                      AT_SYMLINK_NOFOLLOW) == 0) {
            if (s.base.empty() && !recovery)
                fail("CONFLICT", "destination appeared");
            check.file = regularFile(check.target.first.get(), check.target.second, O_RDONLY);
            io(::fstat(check.file.get(), &check.identity) == 0);
        } else {
            io(errno == ENOENT);
            if (!s.base.empty())
                fail("CONFLICT", "destination disappeared");
        }
        if (!s.base.empty()) {
            DStmt q(db, "SELECT size,sha256 FROM versions WHERE id=?");
            q.text(1, s.base);
            io(q.row());
            check.size = q.number(0);
            check.sha = q.str(1);
        }
        return check;
    }
    void verifyBase(const S &s, const BaseSnapshot &check) {
        if (s.base.empty()) {
            if (check.file)
                fail("CONFLICT", "destination appeared");
        } else if (!check.file || check.identity.st_size != static_cast<off_t>(check.size) ||
                   hashFd(check.file.get(), check.size) != check.sha)
            fail("CONFLICT", "destination externally modified");
    }
    void unchangedTarget(const BaseSnapshot &check) {
        struct stat now{};
        const auto status = ::fstatat(check.target.first.get(), check.target.second.c_str(), &now,
                                     AT_SYMLINK_NOFOLLOW);
        if (!check.file) {
            if (status == 0) fail("CONFLICT", "destination appeared");
            io(errno == ENOENT);
            return;
        }
        const auto &before = check.identity;
        if (status != 0 || now.st_dev != before.st_dev || now.st_ino != before.st_ino ||
            now.st_size != before.st_size ||
            now.st_mtim.tv_sec != before.st_mtim.tv_sec ||
            now.st_mtim.tv_nsec != before.st_mtim.tv_nsec ||
            now.st_ctim.tv_sec != before.st_ctim.tv_sec ||
            now.st_ctim.tv_nsec != before.st_ctim.tv_nsec)
            fail("CONFLICT", "destination changed during commit");
    }
    protocol::DeltaResult result(const S &s) {
        DStmt q(db, "SELECT result FROM delta_sessions WHERE id=?");
        q.text(1, s.id);
        io(q.row());
        return protocol::decodeDeltaResult(q.str(0));
    }
    protocol::DeltaResult publish(S s) {
        BaseSnapshot check;
        UniqueFd src;
        {
            std::scoped_lock guard(u.mutex, u.dbMutex);
            s = load(s.id);
            if (s.phase == protocol::DeltaPhase::Committed)
                return result(s);
            if (s.phase != protocol::DeltaPhase::Committing)
                fail("CONFLICT", "publish phase changed");
            check = checkBase(s, true);
            src = objectFile(s.version);
        }
        const auto &b = s.b.metadata;
        io(hashFd(src.get(), b.size) == b.sha256);
        const bool already = check.file &&
            check.identity.st_size == static_cast<off_t>(b.size) &&
            hashFd(check.file.get(), b.size) == b.sha256;
        const auto tmp = ".publish-" + s.id;
        UniqueFd prepared;
        if (!already) {
            verifyBase(s, check);
            prepared = regularFile(staging.get(), tmp, O_CREAT | O_TRUNC | O_RDWR, 0600);
            copyFd(src.get(), prepared.get(), b.size);
        }
        {
            std::scoped_lock guard(u.mutex, u.dbMutex);
            const auto version = current(b.path);
            if (version != s.base && version != s.version)
                fail("CONFLICT", "baseline changed before publication");
            unchangedTarget(check);
            if (!already) {
                if (version != s.base)
                    fail("CONFLICT", "published baseline mismatch");
                if (s.base.empty()) {
                    io(::syscall(SYS_renameat2, staging.get(), tmp.c_str(), check.target.first.get(),
                                 check.target.second.c_str(), RENAME_NOREPLACE) == 0);
                } else
                    io(::renameat(staging.get(), tmp.c_str(), check.target.first.get(),
                                  check.target.second.c_str()) == 0);
                check.file = std::move(prepared);
                io(::fstat(check.file.get(), &check.identity) == 0);
            }
        }
        sync(check.target.first.get());
        sync(staging.get());
        protocol::DeltaResult r{dip(s.id), dip(s.version), b.path, b.size, b.sha256, true};
        {
            std::scoped_lock guard(u.mutex, u.dbMutex);
            unchangedTarget(check);
            dsql(db, "BEGIN IMMEDIATE");
            try {
                const auto version = current(b.path);
                if (version != s.base && version != s.version)
                    fail("CONFLICT", "baseline changed during recovery");
                DStmt c(db, "INSERT INTO current_files(path,version_id) VALUES(?,?) ON CONFLICT(path) "
                            "DO UPDATE SET version_id=excluded.version_id");
                c.text(1, b.path).text(2, s.version).row();
                DStmt q(db, "UPDATE delta_sessions SET phase=5,result=? WHERE id=?");
                q.text(1, protocol::encodeDeltaResult(r)).text(2, s.id).row();
                dsql(db, "COMMIT");
            } catch (...) {
                dsql(db, "ROLLBACK");
                throw;
            }
            u.paths.erase(b.path);
        }
        cleanupCommitted(s.id);
        return r;
    }

};
DeltaStorage::DeltaStorage(UploadStorage &u) : impl_(std::make_unique<Impl>(*u.impl_)) {}
DeltaStorage::~DeltaStorage() = default;
bool DeltaStorage::contains(const protocol::SessionId &id) {
    auto &i = *impl_;
    std::scoped_lock g(i.u.mutex, i.u.dbMutex);
    DStmt q(i.db, "SELECT 1 FROM delta_sessions WHERE id=?");
    q.text(1, did(id));
    return q.row();
}
protocol::DeltaReady DeltaStorage::begin(const protocol::BeginDelta &b) {
    auto &i = *impl_;
    protocol::encodeBeginDelta(b);
    quotaGeometry(b.metadata);
    i.legacy(b.metadata.path);
    std::scoped_lock g(i.u.mutex, i.u.dbMutex);
    // Bound online maintenance work; startup still drains the complete backlog.
    // This is a cleanup batch size, not a change to upload quotas or protocol limits.
    i.expire(32);
    // A legacy upload can finish between the snapshot and this lock acquisition.
    if (i.legacyPending(b.metadata.path)) fail("BUSY", "legacy index needs retry");
    std::string base = i.current(b.metadata.path);
    if ((b.base.kind == protocol::BaseSelector::Kind::Absent && !base.empty()) ||
        (b.base.kind == protocol::BaseSelector::Kind::Exact && base != did(b.base.versionId)))
        fail("CONFLICT", "baseline mismatch");
    if (b.mode == protocol::DeltaMode::Upload && i.u.paths.count(b.metadata.path))
        fail("CONFLICT", "target busy");
    // Preview quotas need only indexed metadata columns, not JSON decoding every
    // retained preview while both shared locks are held. Upload sizes are decoded
    // only for the bounded set of active uploads; no JSON1/schema dependency.
    DStmt counts(i.db, "SELECT mode,phase,count(*) FROM delta_sessions "
                      "WHERE mode=1 OR phase<5 GROUP BY mode,phase");
    std::uint64_t bytes = 0, uploads = 0, previews = 0, totalPreviews = 0;
    while (counts.row()) {
        const auto count = static_cast<std::uint64_t>(counts.number(2));
        if (counts.number(0) == 1) {
            totalPreviews += count;
            if (counts.number(1) == 0) previews += count;
        } else {
            uploads += count;
        }
    }
    DStmt q(i.db, "SELECT request FROM delta_sessions WHERE mode=0 AND phase<5");
    while (q.row()) {
        auto a = protocol::decodeBeginDelta(q.str(0));
        bytes += a.metadata.size;
    }
    if ((b.mode == protocol::DeltaMode::Preview &&
         (previews >= 8 || totalPreviews >= 20000 ||
          i.previewBytes() + protocol::encodeBeginDelta(b).size() + 256 > 64ULL * 1024 * 1024)) ||
        (b.mode == protocol::DeltaMode::Upload &&
         (uploads + i.u.count >= 32 ||
          bytes + i.u.reserved + b.metadata.size > 512ULL * 1024 * 1024)))
        fail("QUOTA_EXCEEDED", "session quota exceeded");
    auto id = randomId();
    DStmt add(i.db, "INSERT INTO delta_sessions(id,request,base,logical_path,mode,phase,created) "
                    "VALUES(?,?,?,?,?,0,strftime('%s','now'))");
    add.text(1, did(id)).text(2, protocol::encodeBeginDelta(b));
    if (base.empty())
        sqlite3_bind_null(add.p, 3);
    else
        add.text(3, base);
    add.text(4, b.metadata.path).num(5, b.mode == protocol::DeltaMode::Preview ? 1 : 0);
    add.row();
    if (b.mode == protocol::DeltaMode::Upload)
        i.u.paths.insert(b.metadata.path);
    return {id, base.empty() ? std::optional<protocol::VersionId>{} : dip(base),
            static_cast<std::uint32_t>(blocks(b.metadata))};
}
protocol::ManifestAck DeltaStorage::appendManifest(const protocol::ManifestPage &p) {
    auto &i = *impl_;
    std::scoped_lock g(i.u.mutex, i.u.dbMutex);
    auto s = i.load(did(p.sessionId));
    if (s.phase != protocol::DeltaPhase::Collecting && s.phase != protocol::DeltaPhase::PlanReady)
        fail("MALFORMED_MESSAGE", "manifest phase mismatch");
    auto body = protocol::encodeManifestPage(p);
    DStmt old(i.db, "SELECT body FROM manifest_pages WHERE session=? AND page=?");
    old.text(1, s.id).num(2, p.pageIndex);
    if (old.row()) {
        if (old.str(0) != body)
            fail("CONFLICT", "manifest retry differs");
        return {p.sessionId, s.page, s.phase};
    }
    if (s.phase != protocol::DeltaPhase::Collecting)
        fail("MALFORMED_MESSAGE", "manifest phase mismatch");
    protocol::validateManifestPageForFile(p, p.sessionId, s.b.metadata.size, s.b.metadata.chunkSize,
                                          s.page);
    if (s.b.mode == protocol::DeltaMode::Preview &&
        i.previewBytes() + body.size() + p.hashes.size() * 128 + 128 > 64ULL * 1024 * 1024)
        fail("QUOTA_EXCEEDED", "preview metadata quota");
    dsql(i.db, "BEGIN IMMEDIATE");
    try {
        DStmt page(i.db, "INSERT INTO manifest_pages VALUES(?,?,?)");
        page.text(1, s.id).num(2, p.pageIndex).text(3, body).row();
        for (std::size_t j = 0; j < p.hashes.size(); ++j) {
            DStmt c(i.db, "INSERT INTO manifest_chunks(session,idx,sha) VALUES(?,?,?)");
            c.text(1, s.id).num(2, p.firstBlockIndex + j).text(3, toHex(p.hashes[j])).row();
        }
        DStmt up(i.db, "UPDATE delta_sessions SET next_page=next_page+1 WHERE id=?");
        up.text(1, s.id).row();
        dsql(i.db, "COMMIT");
    } catch (...) {
        dsql(i.db, "ROLLBACK");
        throw;
    }
    return {p.sessionId, s.page + 1, s.phase};
}
protocol::ManifestAck DeltaStorage::finishManifest(const protocol::ManifestDone &d) {
    auto &i = *impl_;
    Impl::SessionIoLease lease{i, did(d.sessionId)};
    Impl::S snapshot;
    UniqueFd baseline;
    std::uint64_t baseSize = 0;
    std::vector<std::string> hashes;
    auto validate = [&](const Impl::S &s) {
        DStmt n(i.db, "SELECT count(*) FROM manifest_chunks WHERE session=?");
        n.text(1, s.id);
        io(n.row());
        if (d.pageCount != s.page || d.blockCount != i.count(s) || n.number(0) != d.blockCount)
            fail("MALFORMED_MESSAGE", "incomplete manifest");
        if (s.phase != protocol::DeltaPhase::Collecting &&
            s.phase != protocol::DeltaPhase::PlanReady)
            fail("MALFORMED_MESSAGE", "manifest phase mismatch");
    };
    {
        std::scoped_lock guard(i.u.mutex, i.u.dbMutex);
        snapshot = i.load(did(d.sessionId));
        validate(snapshot);
        if (snapshot.phase == protocol::DeltaPhase::PlanReady)
            return {d.sessionId, snapshot.page, snapshot.phase};
        lease.pin();
        if (snapshot.b.mode == protocol::DeltaMode::Preview &&
            i.previewBytes() + i.count(snapshot) * 32ULL > 64ULL * 1024 * 1024)
            fail("QUOTA_EXCEEDED", "preview plan quota");
        if (!snapshot.base.empty()) {
            DStmt version(i.db, "SELECT size,chunk_size FROM versions WHERE id=?");
            version.text(1, snapshot.base);
            io(version.row());
            baseSize = version.number(0);
            if (version.number(1) == snapshot.b.metadata.chunkSize)
                baseline = i.objectFile(snapshot.base);
        }
        // No SQLite statement or reference to shared state survives this scope.
        // The immutable object FD pins the negotiated baseline across publication.
        hashes.reserve(d.blockCount);
        DStmt chunks(i.db, "SELECT idx,sha FROM manifest_chunks WHERE session=? ORDER BY idx");
        chunks.text(1, snapshot.id);
        while (chunks.row()) {
            io(chunks.number(0) == static_cast<std::int64_t>(hashes.size()));
            hashes.push_back(chunks.str(1));
        }
    }
    // Bounded by the existing 65,536-block geometry. File I/O and hashing run
    // without the storage/DB locks or an open SQLite transaction.
    std::vector<bool> missing(hashes.size(), true);
    const auto &metadata = snapshot.b.metadata;
    for (std::size_t index = 0; index < hashes.size(); ++index) {
        const auto offset = std::uint64_t(index) * metadata.chunkSize;
        const auto length = std::min<std::uint64_t>(metadata.chunkSize, metadata.size - offset);
        if (!baseline || offset > baseSize || length > baseSize - offset ||
            std::min<std::uint64_t>(metadata.chunkSize, baseSize - offset) != length)
            continue;
        std::string bytes(length, '\0');
        std::size_t done = 0;
        while (done < bytes.size()) {
            const auto count = ::pread(baseline.get(), bytes.data() + done, bytes.size() - done,
                                       static_cast<off_t>(offset + done));
            if (count < 0 && errno == EINTR)
                continue;
            io(count > 0);
            done += static_cast<std::size_t>(count);
        }
        const auto digest = sha256(bytes);
        Sha256Digest expected{};
        const auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
        for (std::size_t n = 0; n < expected.size(); ++n)
            expected[n] = static_cast<std::uint8_t>(
                (digit(hashes[index][2 * n]) << 4) | digit(hashes[index][2 * n + 1]));
        const client_core::BlockDescriptor source{static_cast<std::uint32_t>(index),
                                                  static_cast<std::uint32_t>(length), expected};
        const client_core::BlockDescriptor prior{source.index, source.length, digest};
        missing[index] = client_core::compareBlock(source, prior) == client_core::BlockAction::Upload;
    }

    std::scoped_lock guard(i.u.mutex, i.u.dbMutex);
    auto s = i.load(snapshot.id);
    validate(s);
    if (s.base != snapshot.base || s.page != snapshot.page ||
        protocol::encodeBeginDelta(s.b) != protocol::encodeBeginDelta(snapshot.b))
        fail("CONFLICT", "manifest changed during planning");
    if (s.phase == protocol::DeltaPhase::PlanReady)
        return {d.sessionId, s.page, s.phase};
    // Other sessions may have consumed preview quota while locks were released.
    if (s.b.mode == protocol::DeltaMode::Preview &&
        i.previewBytes() + i.count(s) * 32ULL > 64ULL * 1024 * 1024)
        fail("QUOTA_EXCEEDED", "preview plan quota");
    dsql(i.db, "BEGIN IMMEDIATE");
    try {
        int range = 0, first = -1;
        for (std::size_t index = 0; index < missing.size(); ++index) {
            DStmt chunk(i.db, "UPDATE manifest_chunks SET missing=? WHERE session=? AND idx=?");
            chunk.num(1, missing[index]).text(2, s.id).num(3, index).row();
            if (missing[index] && first < 0)
                first = static_cast<int>(index);
            if (first >= 0 && (!missing[index] || index + 1 == missing.size())) {
                const auto end = missing[index] ? index + 1 : index;
                DStmt entry(i.db, "INSERT INTO delta_plan_ranges VALUES(?,?,?,?)");
                entry.text(1, s.id).num(2, range++).num(3, first).num(4, end).row();
                first = -1;
            }
        }
        i.phase(s, protocol::DeltaPhase::PlanReady);
        dsql(i.db, "COMMIT");
    } catch (...) {
        dsql(i.db, "ROLLBACK");
        throw;
    }
    return {d.sessionId, s.page, s.phase};
}
DeltaStorage::PlanPageOrDone DeltaStorage::planPage(const protocol::GetPlanPage &p) {
    auto &i = *impl_;
    // Declared before lock scopes so lease cleanup never recursively locks them.
    Impl::SessionIoLease lease{i, did(p.sessionId)};
    Impl::S snapshot;
    protocol::PlanDone done;
    UniqueFd out, base;
    std::vector<std::pair<std::uint32_t, std::string>> reused;
    {
        std::scoped_lock guard(i.u.mutex, i.u.dbMutex);
        auto s = i.load(did(p.sessionId));
        if (s.phase != protocol::DeltaPhase::PlanReady && s.phase != protocol::DeltaPhase::Transferring)
            fail("MALFORMED_MESSAGE", "plan phase mismatch");
        DStmt n(i.db, "SELECT count(*) FROM delta_plan_ranges WHERE session=?");
        n.text(1, s.id);
        io(n.row());
        auto pages = static_cast<std::uint32_t>((n.number(0) + 511) / 512);
        if (p.pageIndex > pages)
            fail("MALFORMED_MESSAGE", "plan page out of range");
        if (p.pageIndex < pages) {
            protocol::PlanPage page{p.sessionId, i.base(s), p.pageIndex, {}};
            DStmt q(i.db, "SELECT first,last FROM delta_plan_ranges WHERE session=? AND idx>=? AND "
                          "idx<? ORDER BY idx");
            q.text(1, s.id).num(2, p.pageIndex * 512).num(3, (p.pageIndex + 1) * 512);
            while (q.row())
                page.missingRanges.push_back(
                    {static_cast<std::uint32_t>(q.number(0)), static_cast<std::uint32_t>(q.number(1))});
            return page;
        }
        done.changeKind = "Added";
        if (!s.base.empty()) {
            DStmt v(i.db, "SELECT size,sha256 FROM versions WHERE id=?");
            v.text(1, s.base);
            io(v.row());
            done.changeKind = v.number(0) == static_cast<std::int64_t>(s.b.metadata.size) &&
                                      v.str(1) == s.b.metadata.sha256
                                  ? "Unchanged"
                                  : "Modified";
        }
        done.sessionId = p.sessionId;
        done.baseVersion = i.base(s);
        done.pageCount = pages;
        done.blockCount = i.count(s);
        DStmt chunks(i.db, "SELECT idx,missing FROM manifest_chunks WHERE session=? ORDER BY idx");
        chunks.text(1, s.id);
        while (chunks.row()) {
            auto length = std::min<std::uint64_t>(
                s.b.metadata.chunkSize, s.b.metadata.size - chunks.number(0) * s.b.metadata.chunkSize);
            if (chunks.number(1)) {
                ++done.missingBlocks;
                done.missingBytes += length;
            } else {
                ++done.reusedBlocks;
                done.reusedBytes += length;
            }
        }
        if (s.b.mode != protocol::DeltaMode::Upload || s.phase != protocol::DeltaPhase::PlanReady)
            return done;
        if (!i.sessionIo.insert(s.id).second)
            fail("BUSY", "session materialization in progress");
        lease.active = true;
        lease.pin();
        snapshot = s;
        // Open under the locks so expiry cannot remove the path before creation.
        out = regularFile(i.staging.get(), s.id, O_CREAT | O_TRUNC | O_RDWR, 0600);
        if (!s.base.empty())
            base = i.objectFile(s.base);
        DStmt q(i.db, "SELECT idx,sha FROM manifest_chunks WHERE session=? AND missing=0 ORDER BY idx");
        q.text(1, s.id);
        while (q.row())
            reused.emplace_back(static_cast<std::uint32_t>(q.number(0)), q.str(1));
    }
    // No SQLite statements or shared-state references are retained during I/O.
    const auto &metadata = snapshot.b.metadata;
    io(::ftruncate(out.get(), metadata.size) == 0);
    for (const auto &chunk : reused) {
        const auto offset = std::uint64_t(chunk.first) * metadata.chunkSize;
        std::string bytes(std::min<std::uint64_t>(metadata.chunkSize, metadata.size - offset), '\0');
        std::size_t pos = 0;
        while (pos < bytes.size()) {
            auto count = ::pread(base.get(), bytes.data() + pos, bytes.size() - pos,
                                 static_cast<off_t>(offset + pos));
            if (count < 0 && errno == EINTR)
                continue;
            io(count > 0);
            pos += static_cast<std::size_t>(count);
        }
        if (toHex(sha256(bytes)) != chunk.second)
            fail("CHECKSUM_MISMATCH", "baseline block damaged");
        pos = 0;
        while (pos < bytes.size()) {
            auto count = ::pwrite(out.get(), bytes.data() + pos, bytes.size() - pos,
                                  static_cast<off_t>(offset + pos));
            if (count < 0 && errno == EINTR)
                continue;
            io(count > 0);
            pos += static_cast<std::size_t>(count);
        }
    }
    sync(out.get());
    sync(i.staging.get());
    {
        std::scoped_lock guard(i.u.mutex, i.u.dbMutex);
        auto current = i.load(snapshot.id);
        if (current.phase != protocol::DeltaPhase::PlanReady ||
            current.base != snapshot.base || current.page != snapshot.page ||
            protocol::encodeBeginDelta(current.b) != protocol::encodeBeginDelta(snapshot.b))
            fail("CONFLICT", "session changed during materialization");
        dsql(i.db, "BEGIN IMMEDIATE");
        try {
            DStmt accepted(i.db,
                           "UPDATE manifest_chunks SET accepted=1 WHERE session=? AND missing=0");
            accepted.text(1, snapshot.id).row();
            i.phase(current, protocol::DeltaPhase::Transferring);
            dsql(i.db, "COMMIT");
        } catch (...) {
            dsql(i.db, "ROLLBACK");
            throw;
        }
    }
    return done;
}
protocol::ChunkAck DeltaStorage::write(const protocol::DataChunk &c) {
    auto &i = *impl_;
    std::scoped_lock g(i.u.mutex, i.u.dbMutex);
    auto s = i.load(did(c.sessionId));
    if (s.b.mode != protocol::DeltaMode::Upload || s.phase != protocol::DeltaPhase::Transferring)
        fail("MALFORMED_MESSAGE", "write phase mismatch");
    if (i.sessionIo.count(s.id))
        fail("BUSY", "session I/O in progress");
    protocol::validateDataChunkRange(c, s.b.metadata.size, s.b.metadata.chunkSize);
    auto index = c.offset / s.b.metadata.chunkSize;
    DStmt q(i.db, "SELECT sha,accepted FROM manifest_chunks WHERE session=? AND idx=?");
    q.text(1, s.id).num(2, index);
    io(q.row());
    if (q.str(0) != toHex(c.sha256) || sha256(c.data) != c.sha256)
        fail("CHECKSUM_MISMATCH", "manifest digest mismatch");
    auto out = i.data(s);
    if (!q.number(1)) {
        std::size_t pos = 0;
        while (pos < c.data.size()) {
            auto w = ::pwrite(out.get(), c.data.data() + pos, c.data.size() - pos, c.offset + pos);
            if (w < 0 && errno == EINTR)
                continue;
            io(w > 0);
            pos += w;
        }
        sync(out.get(), true);
        DStmt a(i.db, "UPDATE manifest_chunks SET accepted=1 WHERE session=? AND idx=?");
        a.text(1, s.id).num(2, index).row();
    }
    return {c.sessionId, c.offset, static_cast<std::uint32_t>(c.data.size()), c.sha256};
}
protocol::DeltaStatus DeltaStorage::query(const protocol::QuerySession &p) {
    auto &i = *impl_;
    Impl::SessionIoLease lease{i, did(p.sessionId)};
    Impl::S snapshot;
    protocol::DeltaStatus out;
    UniqueFd fd;
    struct Chunk {
        std::uint32_t index;
        std::string sha;
        bool accepted;
    };
    std::vector<Chunk> chunks;
    std::vector<std::uint8_t> bits;
    {
        std::scoped_lock guard(i.u.mutex, i.u.dbMutex);
        auto s = i.load(lease.id);
        out.sessionId = p.sessionId;
        out.mode = s.b.mode;
        out.phase = s.phase;
        out.baseVersion = i.base(s);
        out.nextManifestPage = s.page;
        if (s.phase == protocol::DeltaPhase::Committing)
            fail("BUSY", "publish recovery pending");
        if (s.phase == protocol::DeltaPhase::Committed)
            out.result = i.result(s);
        if (s.phase != protocol::DeltaPhase::Transferring &&
            s.phase != protocol::DeltaPhase::Verifying)
            return out;
        if (!i.sessionIo.insert(s.id).second)
            fail("BUSY", "session I/O in progress");
        lease.active = true;
        lease.pin();
        snapshot = s;
        fd = i.data(s);
        bits.resize((i.count(s) + 7) / 8);
        chunks.reserve(i.count(s));
        DStmt q(i.db, "SELECT idx,sha,accepted FROM manifest_chunks WHERE session=? ORDER BY idx");
        q.text(1, s.id);
        while (q.row()) {
            io(q.number(0) == static_cast<std::int64_t>(chunks.size()));
            chunks.push_back({static_cast<std::uint32_t>(q.number(0)), q.str(1), q.number(2) != 0});
        }
        io(chunks.size() == i.count(s));
    }
    std::vector<std::uint32_t> invalid;
    for (const auto &chunk : chunks) {
        bool valid = chunk.accepted;
        if (valid) {
            const auto offset = std::uint64_t(chunk.index) * snapshot.b.metadata.chunkSize;
            std::string bytes(std::min<std::uint64_t>(snapshot.b.metadata.chunkSize,
                                                    snapshot.b.metadata.size - offset), '\0');
            std::size_t pos = 0;
            while (pos < bytes.size()) {
                auto count = ::pread(fd.get(), bytes.data() + pos, bytes.size() - pos,
                                     static_cast<off_t>(offset + pos));
                if (count < 0 && errno == EINTR)
                    continue;
                if (count <= 0)
                    break;
                pos += static_cast<std::size_t>(count);
            }
            valid = pos == bytes.size() && toHex(sha256(bytes)) == chunk.sha;
            if (!valid)
                invalid.push_back(chunk.index);
        }
        if (!valid)
            bits[chunk.index / 8] |= static_cast<std::uint8_t>(1U << (chunk.index % 8));
    }
    {
        std::scoped_lock guard(i.u.mutex, i.u.dbMutex);
        auto current = i.load(snapshot.id);
        if (current.phase != snapshot.phase || current.base != snapshot.base ||
            current.page != snapshot.page ||
            protocol::encodeBeginDelta(current.b) != protocol::encodeBeginDelta(snapshot.b))
            fail("CONFLICT", "session changed during query");
        if (!invalid.empty()) {
            dsql(i.db, "BEGIN IMMEDIATE");
            try {
                for (auto index : invalid) {
                    DStmt forget(i.db, "UPDATE manifest_chunks SET accepted=0 WHERE session=? AND idx=?");
                    forget.text(1, current.id).num(2, index).row();
                }
                dsql(i.db, "COMMIT");
            } catch (...) {
                dsql(i.db, "ROLLBACK");
                throw;
            }
        }
    }
    for (auto byte : bits) {
        out.missingBitmap += "0123456789abcdef"[byte >> 4];
        out.missingBitmap += "0123456789abcdef"[byte & 15];
    }
    return out;
}
protocol::DeltaResult DeltaStorage::commit(const protocol::CommitUpload &p) {
    auto &i = *impl_;
    Impl::SessionIoLease lease{i, did(p.sessionId)};
    Impl::S snapshot;
    Impl::BaseSnapshot base;
    UniqueFd fd;
    {
        std::scoped_lock guard(i.u.mutex, i.u.dbMutex);
        snapshot = i.load(lease.id);
        if (snapshot.phase == protocol::DeltaPhase::Committed)
            return i.result(snapshot);
        if (snapshot.b.mode != protocol::DeltaMode::Upload ||
            (snapshot.phase != protocol::DeltaPhase::Transferring &&
             snapshot.phase != protocol::DeltaPhase::Committing))
            fail("MALFORMED_MESSAGE", "commit phase mismatch");
        if (!i.sessionIo.insert(snapshot.id).second)
            fail("BUSY", "session I/O in progress");
        lease.active = true;
        lease.pin();
        if (snapshot.phase == protocol::DeltaPhase::Transferring) {
            DStmt q(i.db, "SELECT count(*) FROM manifest_chunks WHERE session=? AND accepted=0");
            q.text(1, snapshot.id);
            io(q.row());
            if (q.number(0))
                fail("INCOMPLETE_UPLOAD", "missing blocks");
            base = i.checkBase(snapshot);
            fd = i.data(snapshot);
            if (!snapshot.base.empty()) {
                DStmt v(i.db, "SELECT size,sha256 FROM versions WHERE id=?");
                v.text(1, snapshot.base);
                io(v.row());
                if (v.number(0) == static_cast<std::int64_t>(snapshot.b.metadata.size) &&
                    v.str(1) == snapshot.b.metadata.sha256)
                    snapshot.version = snapshot.base;
            }
        }
    }
    if (snapshot.phase == protocol::DeltaPhase::Transferring) {
        if (hashFd(fd.get(), snapshot.b.metadata.size) != snapshot.b.metadata.sha256)
            fail("CHECKSUM_MISMATCH", "whole file mismatch");
        i.verifyBase(snapshot, base);
        if (snapshot.version.empty())
            snapshot.version = i.reservePreparation(snapshot);
        i.object(snapshot.version, fd.get(), snapshot.b.metadata.size);
        if (snapshot.version != snapshot.base)
            i.readyPreparation(snapshot.id);
        std::scoped_lock guard(i.u.mutex, i.u.dbMutex);
        const auto current = i.load(snapshot.id);
        if (current.phase != protocol::DeltaPhase::Transferring ||
            current.base != snapshot.base ||
            protocol::encodeBeginDelta(current.b) != protocol::encodeBeginDelta(snapshot.b) ||
            i.current(snapshot.b.metadata.path) != snapshot.base)
            fail("CONFLICT", "baseline changed during commit");
        i.unchangedTarget(base);
        dsql(i.db, "BEGIN IMMEDIATE");
        try {
            DStmt v(i.db, "INSERT OR IGNORE INTO versions VALUES(?,?,?,?,?)");
            v.text(1, snapshot.version).text(2, snapshot.b.metadata.path)
                .num(3, snapshot.b.metadata.size).num(4, snapshot.b.metadata.chunkSize)
                .text(5, snapshot.b.metadata.sha256).row();
            // Reused content keeps its original immutable block geometry.
            if (snapshot.version != snapshot.base) {
                DStmt vc(i.db, "INSERT INTO version_chunks(version_id,idx,sha256,length) SELECT "
                              "?,idx,sha,MIN(?,?-idx*?) FROM manifest_chunks WHERE session=?");
                vc.text(1, snapshot.version).num(2, snapshot.b.metadata.chunkSize)
                    .num(3, snapshot.b.metadata.size).num(4, snapshot.b.metadata.chunkSize)
                    .text(5, snapshot.id).row();
            }
            DStmt update(i.db, "UPDATE delta_sessions SET phase=4,version_id=? WHERE id=?");
            update.text(1, snapshot.version).text(2, snapshot.id).row();
            dsql(i.db, "COMMIT");
        } catch (...) {
            dsql(i.db, "ROLLBACK");
            throw;
        }
    }
    return i.publish(snapshot);
}
