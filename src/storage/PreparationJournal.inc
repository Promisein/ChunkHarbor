// Included inside DeltaStorage::Impl. The journal is independent of SQLite rollback.
struct Preparation {
    std::string session, version, state;
    protocol::BeginDelta request;
};
UniqueFd preparationDir;
std::map<std::string, Preparation> preparations;

static bool preparationId(const std::string &id) {
    return id.size() == 32 && id.find_first_not_of("0123456789abcdef") == std::string::npos;
}
void persistPreparation(const Preparation &p) {
    using J = nlohmann::json;
    const auto bytes = J{{"format", 1}, {"session", p.session}, {"version", p.version},
                         {"state", p.state},
                         {"request", J::parse(protocol::encodeBeginDelta(p.request))}}.dump();
    io(bytes.size() <= protocol::MaxControlPayload + 1024);
    const auto temporary = ".tmp-" + p.session;
    auto file = regularFile(preparationDir.get(), temporary, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    append(file.get(), bytes);
    sync(file.get());
    io(::renameat(preparationDir.get(), temporary.c_str(), preparationDir.get(), p.session.c_str()) == 0);
    sync(preparationDir.get());
}
void forgetPreparation(const std::string &id) {
    // Caller holds both locks, or is in serialized startup recovery.
    if (::unlinkat(preparationDir.get(), id.c_str(), 0) < 0) io(errno == ENOENT);
    if (::unlinkat(preparationDir.get(), (".tmp-" + id).c_str(), 0) < 0) io(errno == ENOENT);
    sync(preparationDir.get());
    preparations.erase(id);
}
void readPreparations() {
    auto scan = openSafe(preparationDir.get(), ".", O_RDONLY | O_DIRECTORY);
    std::unique_ptr<DIR, decltype(&::closedir)> entries(::fdopendir(scan.get()), ::closedir);
    io(entries != nullptr);
    scan.release();
    std::set<std::string> versionsSeen;
    errno = 0;
    while (auto *entry = ::readdir(entries.get())) {
        const std::string name(entry->d_name);
        if (name == "." || name == "..") continue;
        if (name.rfind(".tmp-", 0) == 0 && preparationId(name.substr(5))) {
            // A temporary journal is never authorization to create an object.
            auto file = regularFile(preparationDir.get(), name, O_RDONLY);
            io(::unlinkat(preparationDir.get(), name.c_str(), 0) == 0);
            errno = 0;
            continue;
        }
        io(preparationId(name));
        auto file = regularFile(preparationDir.get(), name, O_RDONLY);
        std::string bytes;
        char buffer[4096];
        for (;;) {
            auto count = ::read(file.get(), buffer, sizeof buffer);
            if (count < 0 && errno == EINTR) continue;
            io(count >= 0);
            if (!count) break;
            bytes.append(buffer, static_cast<std::size_t>(count));
            io(bytes.size() <= protocol::MaxControlPayload + 1024);
        }
        const auto j = nlohmann::json::parse(bytes);
        io(j.is_object() && j.size() == 5 && j.at("format").is_number_integer() &&
           j.at("format") == 1);
        Preparation p{j.at("session").get<std::string>(), j.at("version").get<std::string>(),
                      j.at("state").get<std::string>(),
                      protocol::decodeBeginDelta(j.at("request").dump())};
        io(p.session == name && preparationId(p.version) &&
           p.request.mode == protocol::DeltaMode::Upload &&
           (p.state == "preparing" || p.state == "ready" || p.state == "reclaiming") &&
           versionsSeen.insert(p.version).second);
        preparations.emplace(name, std::move(p));
        errno = 0;
    }
    io(errno == 0);
    sync(preparationDir.get());
}
bool preparationReferenced(const std::string &version) {
    DStmt q(db, "SELECT 1 FROM versions WHERE id=?1 "
               "UNION ALL SELECT 1 FROM current_files WHERE version_id=?1 "
               "UNION ALL SELECT 1 FROM version_history WHERE version_id=?1 "
               "UNION ALL SELECT 1 FROM snapshot_entries WHERE version_id=?1 "
               "UNION ALL SELECT 1 FROM legacy_versions WHERE version_id=?1 "
               "UNION ALL SELECT 1 FROM transfers WHERE session_id=?1 "
               "UNION ALL SELECT 1 FROM delta_sessions WHERE version_id=?1 LIMIT 1");
    q.text(1, version);
    return q.row();
}
void validatePreparationOwner(const Preparation &p) {
    DStmt owner(db, "SELECT request FROM delta_sessions WHERE id=?");
    owner.text(1, p.session);
    if (owner.row() && protocol::encodeBeginDelta(protocol::decodeBeginDelta(owner.str(0))) !=
                       protocol::encodeBeginDelta(p.request))
        fail("CONFLICT", "preparation session identity mismatch");
}
void reclaimPreparation(const std::string &id) {
    // Serialized with registration by the shared locks. Never reclaim a live lease.
    auto entry = preparations.find(id);
    if (entry == preparations.end()) return;
    if (activePins.count(id)) fail("BUSY", "preparation is in use");
    auto &p = entry->second;
    validatePreparationOwner(p);
    if (preparationReferenced(p.version))
        fail("CONFLICT", "preparation has persistent references");
    p.state = "reclaiming";
    persistPreparation(p);
    struct stat info{};
    if (::fstatat(versions.get(), p.version.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0) {
        auto folder = openSafe(versions.get(), p.version, O_RDONLY | O_DIRECTORY);
        io(::fstat(folder.get(), &info) == 0 && info.st_uid == ::geteuid() &&
           (info.st_mode & 0077) == 0);
        auto scan = openSafe(folder.get(), ".", O_RDONLY | O_DIRECTORY);
        std::unique_ptr<DIR, decltype(&::closedir)> contents(::fdopendir(scan.get()), ::closedir);
        io(contents != nullptr);
        scan.release();
        std::vector<std::string> children;
        errno = 0;
        while (auto *child = ::readdir(contents.get())) {
            const std::string name(child->d_name);
            if (name == "." || name == "..") continue;
            io(name == "data" || name == "pending");
            auto checked = regularFile(folder.get(), name, O_RDONLY);
            children.push_back(name);
            errno = 0;
        }
        io(errno == 0);
        for (const auto &name : children)
            io(::unlinkat(folder.get(), name.c_str(), 0) == 0);
        sync(folder.get());
        io(::unlinkat(versions.get(), p.version.c_str(), AT_REMOVEDIR) == 0);
    } else io(errno == ENOENT);
    sync(versions.get());
    forgetPreparation(id);
}
void recoverPreparations() {
    std::vector<std::string> ids;
    for (const auto &entry : preparations) ids.push_back(entry.first);
    for (const auto &id : ids) {
        try {
            const auto p = preparations.at(id);
            validatePreparationOwner(p);
            DStmt version(db, "SELECT path,size,chunk_size,sha256 FROM versions WHERE id=?");
            version.text(1, p.version);
            if (version.row()) {
                DStmt owner(db, "SELECT phase,version_id FROM delta_sessions WHERE id=?");
                owner.text(1, id);
                io(owner.row() && (owner.number(0) == 4 || owner.number(0) == 5) &&
                   owner.str(1) == p.version && version.str(0) == p.request.metadata.path &&
                   version.number(1) == static_cast<std::int64_t>(p.request.metadata.size) &&
                   version.number(2) == p.request.metadata.chunkSize &&
                   version.str(3) == p.request.metadata.sha256);
                if (owner.number(0) == 5) forgetPreparation(id);
            } else {
                reclaimPreparation(id);
            }
        } catch (const std::exception &error) {
            throw std::runtime_error("preparation recovery failed for session " + id + ": " + error.what());
        }
    }
}
std::string reservePreparation(const S &s) {
    Preparation p;
    {
        std::scoped_lock guard(u.mutex, u.dbMutex);
        auto existing = preparations.find(s.id);
        if (existing != preparations.end()) {
            p = existing->second;
            io(protocol::encodeBeginDelta(p.request) == protocol::encodeBeginDelta(s.b));
            if (p.state == "reclaiming") fail("BUSY", "preparation reclamation pending");
        } else {
            // Reserve across workers before the journal I/O; a failed journal write
            // retains only one in-memory reservation for this bounded live session.
            bool available = false;
            while (!available) {
                p.version = did(randomId());
                struct stat info{};
                const auto status = ::fstatat(versions.get(), p.version.c_str(), &info, AT_SYMLINK_NOFOLLOW);
                io(status == 0 || errno == ENOENT);
                available = status != 0 && !preparationReferenced(p.version) &&
                    std::none_of(preparations.begin(), preparations.end(),
                        [&](const auto &entry) { return entry.second.version == p.version; });
            }
            p.session = s.id;
            p.request = s.b;
            p.state = "preparing";
            preparations.emplace(s.id, p);
        }
    }
    persistPreparation(p);
    return p.version;
}
void readyPreparation(const std::string &id) {
    Preparation p;
    {
        std::scoped_lock guard(u.mutex, u.dbMutex);
        p = preparations.at(id);
    }
    p.state = "ready";
    persistPreparation(p);
    std::scoped_lock guard(u.mutex, u.dbMutex);
    preparations.at(id) = std::move(p);
}
