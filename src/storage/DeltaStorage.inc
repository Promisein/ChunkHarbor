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
    struct S {
        std::string id, base, version;
        protocol::BeginDelta b;
        protocol::DeltaPhase phase;
        std::uint32_t page;
    };
    explicit Impl(UploadStorage::Impl &store) : u(store), db(store.db->nativeHandle()) {
        versions = directory(u.root.get(), ".versions");
        staging = directory(u.root.get(), ".delta");
        sync(u.root.get());
        expire();
        legacy();
        isolateObjects();
        DStmt q(db, "SELECT id FROM delta_sessions WHERE phase=4");
        std::vector<std::string> pending;
        while (q.row())
            pending.push_back(q.str(0));
        for (auto &id : pending)
            publish(load(id));
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
    void expire() {
        DStmt q(db, "SELECT id,request,created FROM delta_sessions WHERE phase<4 OR phase=7");
        std::vector<std::pair<std::string, std::string>> expired;
        auto now = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
        while (q.row()) {
            auto b = protocol::decodeBeginDelta(q.str(1));
            if (now - q.number(2) > (b.mode == protocol::DeltaMode::Preview ? 86400 : 2592000))
                expired.push_back(
                    {q.str(0), b.mode == protocol::DeltaMode::Upload ? b.metadata.path : ""});
        }
        for (auto &item : expired) {
            DStmt up(db, "UPDATE delta_sessions SET phase=7 WHERE id=?");
            up.text(1, item.first).row();
            for (const char *table : {"manifest_pages", "manifest_chunks", "delta_plan_ranges"}) {
                std::string sql = std::string("DELETE FROM ") + table + " WHERE session=?";
                DStmt clean(db, sql.c_str());
                clean.text(1, item.first).row();
            }
            if (item.second.empty()) {
                DStmt clean(db, "DELETE FROM delta_sessions WHERE id=?");
                clean.text(1, item.first).row();
            }
            if (::unlinkat(staging.get(), item.first.c_str(), 0) < 0)
                io(errno == ENOENT);
            if (!item.second.empty())
                u.paths.erase(item.second);
        }
        if (!expired.empty())
            sync(staging.get());
    }
    void legacy() {
        for (const auto &t : u.db->transfers()) {
            if (t.state != "COMMITTED")
                continue;
            DStmt found(db, "SELECT version_id FROM legacy_versions WHERE session_id=?");
            found.text(1, t.sessionId);
            if (found.row())
                continue;
            auto target = u.target(t.logicalPath);
            auto src = regularFile(target.first.get(), target.second, O_RDONLY);
            io(hashFd(src.get(), t.totalSize) == t.expectedSha256);
            object(t.sessionId, src.get(), t.totalSize);
            dsql(db, "BEGIN IMMEDIATE");
            try {
                DStmt v(db, "INSERT OR IGNORE INTO versions VALUES(?,?,?,?,?)");
                v.text(1, t.sessionId)
                    .text(2, t.logicalPath)
                    .num(3, t.totalSize)
                    .num(4, t.chunkSize)
                    .text(5, t.expectedSha256)
                    .row();
                for (std::uint64_t off = 0, index = 0;
                     off < static_cast<std::uint64_t>(t.totalSize); off += t.chunkSize, ++index) {
                    std::string bytes(std::min<std::uint64_t>(t.chunkSize, t.totalSize - off),
                                      '\0');
                    std::size_t pos = 0;
                    while (pos < bytes.size()) {
                        auto n =
                            ::pread(src.get(), bytes.data() + pos, bytes.size() - pos, off + pos);
                        if (n < 0 && errno == EINTR)
                            continue;
                        io(n > 0);
                        pos += n;
                    }
                    DStmt chunk(db, "INSERT INTO version_chunks VALUES(?,?,?,?)");
                    chunk.text(1, t.sessionId)
                        .num(2, index)
                        .text(3, toHex(sha256(bytes)))
                        .num(4, bytes.size())
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
        if (s.phase != protocol::DeltaPhase::Committed &&
            s.phase != protocol::DeltaPhase::Committing &&
            now - q.number(5) > (s.b.mode == protocol::DeltaMode::Preview ? 86400 : 2592000)) {
            phase(s, protocol::DeltaPhase::Expired);
            if (s.b.mode == protocol::DeltaMode::Upload)
                u.paths.erase(s.b.metadata.path);
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
    void checkBase(const S &s) {
        if (current(s.b.metadata.path) != s.base)
            fail("CONFLICT", "baseline changed");
        auto t = u.target(s.b.metadata.path);
        struct stat st{};
        if (s.base.empty()) {
            if (::fstatat(t.first.get(), t.second.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0)
                fail("CONFLICT", "destination appeared");
            io(errno == ENOENT);
        } else {
            auto f = regularFile(t.first.get(), t.second, O_RDONLY);
            DStmt q(db, "SELECT size,sha256 FROM versions WHERE id=?");
            q.text(1, s.base);
            io(q.row());
            io(::fstat(f.get(), &st) == 0);
            if (st.st_size != q.number(0) || hashFd(f.get(), q.number(0)) != q.str(1))
                fail("CONFLICT", "destination externally modified");
        }
    }
    protocol::DeltaResult result(const S &s) {
        DStmt q(db, "SELECT result FROM delta_sessions WHERE id=?");
        q.text(1, s.id);
        io(q.row());
        return protocol::decodeDeltaResult(q.str(0));
    }
    protocol::DeltaResult publish(S s) {
        auto &b = s.b.metadata;
        auto target = u.target(b.path);
        auto src = objectFile(s.version);
        io(hashFd(src.get(), b.size) == b.sha256);
        struct stat st{};
        bool already = false;
        if (::fstatat(target.first.get(), target.second.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0) {
            auto f = regularFile(target.first.get(), target.second, O_RDONLY);
            already =
                st.st_size == static_cast<off_t>(b.size) && hashFd(f.get(), b.size) == b.sha256;
        }
        if (!already) {
            checkBase(s);
            auto tmp = ".publish-" + s.id;
            auto out = regularFile(staging.get(), tmp, O_CREAT | O_TRUNC | O_RDWR, 0600);
            copyFd(src.get(), out.get(), b.size);
            if (s.base.empty()) {
                io(::syscall(SYS_renameat2, staging.get(), tmp.c_str(), target.first.get(),
                             target.second.c_str(), RENAME_NOREPLACE) == 0);
            } else
                io(::renameat(staging.get(), tmp.c_str(), target.first.get(),
                              target.second.c_str()) == 0);
            sync(target.first.get());
            sync(staging.get());
        }
        sync(target.first.get());
        protocol::DeltaResult r{dip(s.id), dip(s.version), b.path, b.size, b.sha256, true};
        dsql(db, "BEGIN IMMEDIATE");
        try {
            if (current(b.path) != s.base && current(b.path) != s.version)
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
        ::unlinkat(staging.get(), s.id.c_str(), 0);
        sync(staging.get());
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
    std::scoped_lock g(i.u.mutex, i.u.dbMutex);
    protocol::encodeBeginDelta(b);
    quotaGeometry(b.metadata);
    i.expire();
    i.legacy();
    std::string base = i.current(b.metadata.path);
    if ((b.base.kind == protocol::BaseSelector::Kind::Absent && !base.empty()) ||
        (b.base.kind == protocol::BaseSelector::Kind::Exact && base != did(b.base.versionId)))
        fail("CONFLICT", "baseline mismatch");
    if (b.mode == protocol::DeltaMode::Upload && i.u.paths.count(b.metadata.path))
        fail("CONFLICT", "target busy");
    DStmt q(i.db, "SELECT request,phase FROM delta_sessions");
    std::uint64_t bytes = 0;
    unsigned uploads = 0, previews = 0, totalPreviews = 0;
    while (q.row()) {
        auto a = protocol::decodeBeginDelta(q.str(0));
        if (a.mode == protocol::DeltaMode::Preview) {
            ++totalPreviews;
            if (q.number(1) == 0)
                ++previews;
        } else if (q.number(1) < 5) {
            ++uploads;
            bytes += a.metadata.size;
        }
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
    std::scoped_lock g(i.u.mutex, i.u.dbMutex);
    auto s = i.load(did(d.sessionId));
    DStmt n(i.db, "SELECT count(*) FROM manifest_chunks WHERE session=?");
    n.text(1, s.id);
    io(n.row());
    if (d.pageCount != s.page || d.blockCount != i.count(s) || n.number(0) != d.blockCount)
        fail("MALFORMED_MESSAGE", "incomplete manifest");
    if (s.phase != protocol::DeltaPhase::Collecting) {
        if (s.phase == protocol::DeltaPhase::PlanReady)
            return {d.sessionId, s.page, s.phase};
        fail("MALFORMED_MESSAGE", "manifest phase mismatch");
    }
    if (s.b.mode == protocol::DeltaMode::Preview &&
        i.previewBytes() + i.count(s) * 32ULL > 64ULL * 1024 * 1024)
        fail("QUOTA_EXCEEDED", "preview plan quota");
    UniqueFd baseline;
    std::uint64_t baseSize = 0;
    bool compatible = false;
    if (!s.base.empty()) {
        DStmt v(i.db, "SELECT size,chunk_size FROM versions WHERE id=?");
        v.text(1, s.base);
        io(v.row());
        baseSize = v.number(0);
        compatible = v.number(1) == s.b.metadata.chunkSize;
        if (compatible)
            baseline = i.objectFile(s.base);
    }
    dsql(i.db, "BEGIN IMMEDIATE");
    try {
        DStmt chunks(i.db, "SELECT idx,sha FROM manifest_chunks WHERE session=? ORDER BY idx");
        chunks.text(1, s.id);
        int range = 0, start = -1, last = -1;
        while (chunks.row()) {
            auto index = chunks.number(0);
            auto offset = index * s.b.metadata.chunkSize;
            auto length =
                std::min<std::uint64_t>(s.b.metadata.chunkSize, s.b.metadata.size - offset);
            bool reuse = false;
            if (compatible && offset + length <= baseSize &&
                std::min<std::uint64_t>(s.b.metadata.chunkSize, baseSize - offset) == length) {
                std::string bytes(length, '\0');
                std::size_t done = 0;
                while (done < bytes.size()) {
                    auto r = ::pread(baseline.get(), bytes.data() + done, bytes.size() - done,
                                     offset + done);
                    if (r < 0 && errno == EINTR)
                        continue;
                    io(r > 0);
                    done += r;
                }
                auto digest = sha256(bytes);
                auto source = digest;
                auto hex = chunks.str(1);
                auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
                for (std::size_t n = 0; n < 32; ++n)
                    source[n] =
                        static_cast<std::uint8_t>((digit(hex[2 * n]) << 4) | digit(hex[2 * n + 1]));
                client_core::BlockDescriptor src{static_cast<std::uint32_t>(index),
                                                 static_cast<std::uint32_t>(length), source},
                    prior{static_cast<std::uint32_t>(index), static_cast<std::uint32_t>(length),
                          digest};
                reuse = client_core::compareBlock(src, prior) == client_core::BlockAction::Reuse;
            }
            DStmt c(i.db, "UPDATE manifest_chunks SET missing=? WHERE session=? AND idx=?");
            c.num(1, !reuse).text(2, s.id).num(3, index).row();
            if (!reuse) {
                if (start < 0)
                    start = static_cast<int>(index);
                last = static_cast<int>(index);
            }
            if ((reuse && start >= 0) || (!reuse && index + 1 == i.count(s))) {
                DStmt r(i.db, "INSERT INTO delta_plan_ranges VALUES(?,?,?,?)");
                r.text(1, s.id).num(2, range++).num(3, start).num(4, last + 1).row();
                start = -1;
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
    std::scoped_lock g(i.u.mutex, i.u.dbMutex);
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
    protocol::PlanDone done;
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
    if (s.b.mode == protocol::DeltaMode::Upload && s.phase == protocol::DeltaPhase::PlanReady) {
        auto out = regularFile(i.staging.get(), s.id, O_CREAT | O_TRUNC | O_RDWR, 0600);
        io(::ftruncate(out.get(), s.b.metadata.size) == 0);
        UniqueFd base;
        if (!s.base.empty())
            base = i.objectFile(s.base);
        DStmt q(i.db,
                "SELECT idx,sha FROM manifest_chunks WHERE session=? AND missing=0 ORDER BY idx");
        q.text(1, s.id);
        while (q.row()) {
            auto offset = q.number(0) * s.b.metadata.chunkSize;
            std::string bytes(
                std::min<std::uint64_t>(s.b.metadata.chunkSize, s.b.metadata.size - offset), '\0');
            std::size_t pos = 0;
            while (pos < bytes.size()) {
                auto r = ::pread(base.get(), bytes.data() + pos, bytes.size() - pos, offset + pos);
                if (r < 0 && errno == EINTR)
                    continue;
                io(r > 0);
                pos += r;
            }
            if (toHex(sha256(bytes)) != q.str(1))
                fail("CHECKSUM_MISMATCH", "baseline block damaged");
            pos = 0;
            while (pos < bytes.size()) {
                auto w = ::pwrite(out.get(), bytes.data() + pos, bytes.size() - pos, offset + pos);
                if (w < 0 && errno == EINTR)
                    continue;
                io(w > 0);
                pos += w;
            }
        }
        sync(out.get());
        sync(i.staging.get());
        dsql(i.db, "BEGIN IMMEDIATE");
        try {
            DStmt accepted(i.db,
                           "UPDATE manifest_chunks SET accepted=1 WHERE session=? AND missing=0");
            accepted.text(1, s.id).row();
            i.phase(s, protocol::DeltaPhase::Transferring);
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
    std::scoped_lock g(i.u.mutex, i.u.dbMutex);
    auto s = i.load(did(p.sessionId));
    protocol::DeltaStatus out;
    out.sessionId = p.sessionId;
    out.mode = s.b.mode;
    out.phase = s.phase;
    out.baseVersion = i.base(s);
    out.nextManifestPage = s.page;
    if (s.phase == protocol::DeltaPhase::Committing)
        fail("BUSY", "publish recovery pending");
    if (s.phase == protocol::DeltaPhase::Committed)
        out.result = i.result(s);
    if (s.phase == protocol::DeltaPhase::Transferring ||
        s.phase == protocol::DeltaPhase::Verifying) {
        std::vector<std::uint8_t> bits((i.count(s) + 7) / 8);
        auto fd = i.data(s);
        DStmt q(i.db, "SELECT idx,sha,accepted FROM manifest_chunks WHERE session=?");
        q.text(1, s.id);
        while (q.row()) {
            auto idx = q.number(0);
            bool valid = q.number(2);
            if (valid) {
                auto off = idx * s.b.metadata.chunkSize;
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
                valid = pos == bytes.size() && toHex(sha256(bytes)) == q.str(1);
                if (!valid) {
                    DStmt forget(i.db,
                                 "UPDATE manifest_chunks SET accepted=0 WHERE session=? AND idx=?");
                    forget.text(1, s.id).num(2, idx).row();
                }
            }
            if (!valid)
                bits[idx / 8] |= static_cast<std::uint8_t>(1U << (idx % 8));
        }
        for (auto b : bits) {
            out.missingBitmap += "0123456789abcdef"[b >> 4];
            out.missingBitmap += "0123456789abcdef"[b & 15];
        }
    }
    return out;
}
protocol::DeltaResult DeltaStorage::commit(const protocol::CommitUpload &p) {
    auto &i = *impl_;
    std::scoped_lock g(i.u.mutex, i.u.dbMutex);
    auto s = i.load(did(p.sessionId));
    if (s.phase == protocol::DeltaPhase::Committed)
        return i.result(s);
    if (s.b.mode != protocol::DeltaMode::Upload || s.phase != protocol::DeltaPhase::Transferring)
        fail("MALFORMED_MESSAGE", "commit phase mismatch");
    DStmt q(i.db, "SELECT count(*) FROM manifest_chunks WHERE session=? AND accepted=0");
    q.text(1, s.id);
    io(q.row());
    if (q.number(0))
        fail("INCOMPLETE_UPLOAD", "missing blocks");
    auto fd = i.data(s);
    if (hashFd(fd.get(), s.b.metadata.size) != s.b.metadata.sha256)
        fail("CHECKSUM_MISMATCH", "whole file mismatch");
    i.checkBase(s);
    if (!s.base.empty()) {
        DStmt v(i.db, "SELECT size,sha256 FROM versions WHERE id=?");
        v.text(1, s.base);
        io(v.row());
        if (v.number(0) == static_cast<std::int64_t>(s.b.metadata.size) &&
            v.str(1) == s.b.metadata.sha256)
            s.version = s.base;
    }
    if (s.version.empty())
        s.version = did(randomId());
    i.object(s.version, fd.get(), s.b.metadata.size);
    dsql(i.db, "BEGIN IMMEDIATE");
    try {
        DStmt v(i.db, "INSERT OR IGNORE INTO versions VALUES(?,?,?,?,?)");
        v.text(1, s.version)
            .text(2, s.b.metadata.path)
            .num(3, s.b.metadata.size)
            .num(4, s.b.metadata.chunkSize)
            .text(5, s.b.metadata.sha256)
            .row();
        // Content identity may be reused after negotiating a different block
        // size. Its existing immutable manifest must retain the original geometry.
        if (s.version != s.base) {
            DStmt vc(i.db, "INSERT INTO version_chunks(version_id,idx,sha256,length) SELECT "
                           "?,idx,sha,MIN(?,?-idx*?) FROM manifest_chunks WHERE session=?");
            vc.text(1, s.version)
                .num(2, s.b.metadata.chunkSize)
                .num(3, s.b.metadata.size)
                .num(4, s.b.metadata.chunkSize)
                .text(5, s.id)
                .row();
        }
        DStmt u(i.db, "UPDATE delta_sessions SET phase=4,version_id=? WHERE id=?");
        u.text(1, s.version).text(2, s.id).row();
        dsql(i.db, "COMMIT");
    } catch (...) {
        dsql(i.db, "ROLLBACK");
        throw;
    }
    return i.publish(s);
}
