#include "chunkharbor/storage/SqliteStore.h"
#include "chunkharbor/protocol/Download.h"
#include <chrono>
#include <fstream>
#include <sqlite3.h>
#include <stdexcept>
#include <string>
#include <vector>
namespace chunkharbor {
namespace {
void exec(sqlite3 *db, const char *sql) {
    char *error = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &error) == SQLITE_OK)
        return;
    std::string message = error ? error : sqlite3_errmsg(db);
    sqlite3_free(error);
    throw std::runtime_error("SQLite: " + message);
}
int integer(sqlite3 *db, const char *sql) {
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db));
    const int step = sqlite3_step(stmt);
    const int value = step == SQLITE_ROW ? sqlite3_column_int(stmt, 0) : -1;
    sqlite3_finalize(stmt);
    if (step != SQLITE_ROW)
        throw std::runtime_error("SQLite pragma returned no row");
    return value;
}
std::string text(sqlite3 *db, const char *sql) {
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db));
    const int step = sqlite3_step(stmt);
    std::string value = step == SQLITE_ROW && sqlite3_column_text(stmt, 0)
                            ? reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0))
                            : "";
    sqlite3_finalize(stmt);
    return value;
}
struct Statement {
    sqlite3_stmt *value = nullptr;
    Statement(sqlite3 *db, const char *sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &value, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
    }
    ~Statement() { sqlite3_finalize(value); }
};
void bindText(sqlite3_stmt *stmt, int index, std::string_view value) {
    if (sqlite3_bind_text(stmt, index, value.data(), static_cast<int>(value.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK)
        throw std::runtime_error("SQLite bind failed");
}
void done(sqlite3 *db, sqlite3_stmt *stmt) {
    if (sqlite3_step(stmt) != SQLITE_DONE)
        throw std::runtime_error(sqlite3_errmsg(db));
}
} // namespace
SqliteStore::SqliteStore(std::string_view databasePath, std::string_view schemaPath) {
    std::string path(databasePath);
    if (sqlite3_threadsafe() == 0)
        throw std::runtime_error("SQLite was built without thread support");
    if (sqlite3_open_v2(path.c_str(), &db_,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        std::string message = sqlite3_errmsg(db_);
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("SQLite open: " + message);
    }
    try {
        if (text(db_, "PRAGMA journal_mode=WAL") != "wal")
            throw std::runtime_error("WAL unavailable");
        exec(db_, "PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON");
        if (integer(db_, "PRAGMA synchronous") != 2 || integer(db_, "PRAGMA foreign_keys") != 1)
            throw std::runtime_error("SQLite durability pragmas unavailable");
        const int version = integer(db_, "PRAGMA user_version");
        if (version == 0) {
            if (integer(db_, "SELECT count(*) FROM sqlite_master WHERE type='table' AND name NOT "
                             "LIKE 'sqlite_%'") != 0)
                throw std::runtime_error("unversioned database is not empty");
            std::ifstream input{std::string(schemaPath)};
            if (!input)
                throw std::runtime_error("schema file unavailable");
            std::string schema((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
            exec(db_, schema.c_str());
            if (integer(db_, "PRAGMA user_version") != 1)
                throw std::runtime_error("schema migration did not set version");
        } else if ((version < 1 || version > 4) ||
                   integer(db_, "SELECT version FROM schema_version") != version) {
            throw std::runtime_error("unsupported SQLite schema version");
        }
        for (int next = integer(db_, "PRAGMA user_version") + 1; next <= 4; ++next) {
            std::string migrationPath(schemaPath);
            migrationPath =
                migrationPath.substr(0, migrationPath.find_last_of("/\\") + 1) + "schema_v" + std::to_string(next) + ".sql";
            std::ifstream migration(migrationPath);
            if (!migration)
                throw std::runtime_error("SQLite migration unavailable: " + migrationPath);
            std::string sql((std::istreambuf_iterator<char>(migration)), {});
            try {
                exec(db_, sql.c_str());
                if (integer(db_, "PRAGMA user_version") != next ||
                    integer(db_, "SELECT version FROM schema_version") != next)
                    throw std::runtime_error("SQLite migration version mismatch");
            } catch (...) {
                sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
                throw;
            }
        }
        exec(db_, "CREATE INDEX IF NOT EXISTS committed_transfers_by_path ON "
                  "transfers(logical_path) WHERE state='COMMITTED'");
    } catch (...) {
        sqlite3_close(db_);
        db_ = nullptr;
        throw;
    }
}
SqliteStore::~SqliteStore() { sqlite3_close(db_); }
std::vector<VersionRecord> SqliteStore::listCurrentFiles(std::string_view afterPath,
                                                       std::uint32_t limit) const {
    if (!limit || limit > 65) throw std::invalid_argument("invalid file list limit");
    // Both branches seek indexed paths. UNION ALL merges bounded ordered branches;
    // current_files wins over a legacy committed transfer for the same path.
    Statement q(db_,
        "SELECT c.path,v.id,v.size,v.chunk_size,v.sha256 FROM current_files c "
        "JOIN versions v ON v.id=c.version_id WHERE c.path>?1 "
        "UNION ALL SELECT t.logical_path,t.session_id,t.total_size,t.chunk_size,t.expected_sha256 "
        "FROM transfers t WHERE t.state='COMMITTED' AND t.logical_path>?1 "
        "AND NOT EXISTS(SELECT 1 FROM current_files c WHERE c.path=t.logical_path) "
        "ORDER BY 1 COLLATE BINARY LIMIT ?2");
    bindText(q.value, 1, afterPath);
    sqlite3_bind_int(q.value, 2, static_cast<int>(limit));
    std::vector<VersionRecord> rows;
    int status;
    while ((status = sqlite3_step(q.value)) == SQLITE_ROW) {
        auto textAt = [&](int col) { return std::string(reinterpret_cast<const char *>(sqlite3_column_text(q.value, col))); };
        rows.push_back({textAt(1), textAt(0), static_cast<std::uint64_t>(sqlite3_column_int64(q.value, 2)),
                       static_cast<std::uint32_t>(sqlite3_column_int(q.value, 3)), textAt(4)});
    }
    if (status != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db_));
    return rows;
}

protocol::VersionsResult SqliteStore::listVersions(const protocol::ListVersions &request) const {
    protocol::encodeListVersions(request);
    Statement query(db_,
        "SELECT h.version_id,COALESCE(v.size,t.total_size),COALESCE(v.sha256,t.expected_sha256),"
        "h.sequence,h.committed_at_s,CASE WHEN c.version_id IS NOT NULL THEN c.version_id=h.version_id "
        "ELSE COALESCE(t.state='COMMITTED',0) END FROM version_history h "
        "LEFT JOIN versions v ON v.id=h.version_id LEFT JOIN transfers t ON t.session_id=h.version_id "
        "LEFT JOIN current_files c ON c.path=h.path WHERE h.path=?1 AND h.sequence<=?2 "
        "ORDER BY h.sequence DESC LIMIT ?3");
    bindText(query.value, 1, request.path);
    sqlite3_bind_int64(query.value, 2, request.before ? static_cast<sqlite3_int64>(*request.before - 1) : INT64_MAX);
    sqlite3_bind_int(query.value, 3, static_cast<int>(request.limit + 1));
    protocol::VersionsResult result{request.path, {}, {}};
    int status;
    while ((status = sqlite3_step(query.value)) == SQLITE_ROW) {
        const auto *hash = sqlite3_column_text(query.value, 2);
        if (!hash) throw std::runtime_error("missing history metadata");
        std::optional<std::uint64_t> time;
        if (sqlite3_column_type(query.value, 4) != SQLITE_NULL)
            time = static_cast<std::uint64_t>(sqlite3_column_int64(query.value, 4));
        result.versions.push_back({
            reinterpret_cast<const char *>(sqlite3_column_text(query.value, 0)),
            static_cast<std::uint64_t>(sqlite3_column_int64(query.value, 1)),
            reinterpret_cast<const char *>(hash),
            static_cast<std::uint64_t>(sqlite3_column_int64(query.value, 3)), time,
            sqlite3_column_int(query.value, 5) != 0});
    }
    if (status != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db_));
    if (result.versions.size() > request.limit) {
        result.versions.pop_back();
        result.nextBefore = result.versions.back().sequence;
    }
    return result;
}
void SqliteStore::createTransfer(const Transfer &t) {
    Statement stmt(
        db_, "INSERT INTO "
             "transfers(session_id,logical_path,expected_sha256,total_size,chunk_size,mtime_ns,"
             "temp_path,state,created_at_ns,updated_at_ns) VALUES (?,?,?,?,?,?,?,'CREATED',?,?)");
    bindText(stmt.value, 1, t.sessionId);
    bindText(stmt.value, 2, t.logicalPath);
    bindText(stmt.value, 3, t.expectedSha256);
    sqlite3_bind_int64(stmt.value, 4, t.totalSize);
    sqlite3_bind_int64(stmt.value, 5, t.chunkSize);
    auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count();
    sqlite3_bind_int64(stmt.value, 6, t.mtimeNs);
    bindText(stmt.value, 7, t.tempPath);
    sqlite3_bind_int64(stmt.value, 8, now);
    sqlite3_bind_int64(stmt.value, 9, now);
    done(db_, stmt.value);
}
void SqliteStore::confirmChunk(std::string_view sessionId, std::int64_t index,
                               std::string_view sha256, std::int64_t durableAtNs) {
    exec(db_, "BEGIN IMMEDIATE");
    try {
        Statement chunk(db_,
                        "INSERT INTO transfer_chunks(session_id,chunk_index,sha256,durable_at_ns) "
                        "VALUES (?,?,?,?)");
        bindText(chunk.value, 1, sessionId);
        sqlite3_bind_int64(chunk.value, 2, index);
        bindText(chunk.value, 3, sha256);
        sqlite3_bind_int64(chunk.value, 4, durableAtNs);
        done(db_, chunk.value);
        Statement transfer(
            db_, "UPDATE transfers SET state='RECEIVING',updated_at_ns=MAX(updated_at_ns,?) WHERE "
                 "session_id=? AND state IN ('CREATED','RECEIVING')");
        sqlite3_bind_int64(transfer.value, 1, durableAtNs);
        bindText(transfer.value, 2, sessionId);
        done(db_, transfer.value);
        if (sqlite3_changes(db_) != 1)
            throw std::runtime_error("transfer is not receiving");
        exec(db_, "COMMIT");
    } catch (...) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
}
std::vector<std::int64_t> SqliteStore::confirmedChunks(std::string_view sessionId) const {
    Statement stmt(
        db_, "SELECT chunk_index FROM transfer_chunks WHERE session_id=? ORDER BY chunk_index");
    bindText(stmt.value, 1, sessionId);
    std::vector<std::int64_t> result;
    for (;;) {
        const int step = sqlite3_step(stmt.value);
        if (step == SQLITE_DONE)
            break;
        if (step != SQLITE_ROW)
            throw std::runtime_error(sqlite3_errmsg(db_));
        result.push_back(sqlite3_column_int64(stmt.value, 0));
    }
    return result;
}
std::vector<SqliteStore::Transfer> SqliteStore::transfers() const {
    Statement stmt(db_, "SELECT "
                        "session_id,logical_path,expected_sha256,state,total_size,chunk_size,mtime_"
                        "ns,temp_path,updated_at_ns FROM transfers");
    std::vector<Transfer> result;
    for (;;) {
        int step = sqlite3_step(stmt.value);
        if (step == SQLITE_DONE)
            break;
        if (step != SQLITE_ROW)
            throw std::runtime_error(sqlite3_errmsg(db_));
        Transfer t;
        t.sessionId = reinterpret_cast<const char *>(sqlite3_column_text(stmt.value, 0));
        t.logicalPath = reinterpret_cast<const char *>(sqlite3_column_text(stmt.value, 1));
        t.expectedSha256 = reinterpret_cast<const char *>(sqlite3_column_text(stmt.value, 2));
        t.state = reinterpret_cast<const char *>(sqlite3_column_text(stmt.value, 3));
        t.totalSize = sqlite3_column_int64(stmt.value, 4);
        t.chunkSize = sqlite3_column_int64(stmt.value, 5);
        t.mtimeNs = sqlite3_column_int64(stmt.value, 6);
        t.tempPath = reinterpret_cast<const char *>(sqlite3_column_text(stmt.value, 7));
        t.updatedAtNs = sqlite3_column_int64(stmt.value, 8);
        result.push_back(std::move(t));
    }
    return result;
}
std::vector<SqliteStore::Chunk> SqliteStore::chunks(std::string_view sessionId) const {
    Statement stmt(
        db_,
        "SELECT chunk_index,sha256 FROM transfer_chunks WHERE session_id=? ORDER BY chunk_index");
    bindText(stmt.value, 1, sessionId);
    std::vector<Chunk> result;
    for (;;) {
        int step = sqlite3_step(stmt.value);
        if (step == SQLITE_DONE)
            break;
        if (step != SQLITE_ROW)
            throw std::runtime_error(sqlite3_errmsg(db_));
        result.push_back({sqlite3_column_int64(stmt.value, 0),
                          reinterpret_cast<const char *>(sqlite3_column_text(stmt.value, 1))});
    }
    return result;
}
void SqliteStore::setState(std::string_view sessionId, std::string_view from, std::string_view to) {
    Statement stmt(db_, "UPDATE transfers SET state=?,updated_at_ns=MAX(updated_at_ns,?) WHERE "
                        "session_id=? AND state=?");
    bindText(stmt.value, 1, to);
    auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count();
    sqlite3_bind_int64(stmt.value, 2, now);
    bindText(stmt.value, 3, sessionId);
    bindText(stmt.value, 4, from);
    done(db_, stmt.value);
    if (sqlite3_changes(db_) != 1)
        throw std::runtime_error("invalid transfer state transition");
}
void SqliteStore::forgetChunk(std::string_view sessionId, std::int64_t index) {
    Statement stmt(db_, "DELETE FROM transfer_chunks WHERE session_id=? AND chunk_index=?");
    bindText(stmt.value, 1, sessionId);
    sqlite3_bind_int64(stmt.value, 2, index);
    done(db_, stmt.value);
}

std::optional<VersionRecord> SqliteStore::currentVersion(std::string_view path) const {
    Statement q(db_, "SELECT v.id,v.path,v.size,v.chunk_size,v.sha256 FROM versions v JOIN "
                     "current_files c ON c.version_id=v.id WHERE c.path=?");
    bindText(q.value, 1, path);
    int step = sqlite3_step(q.value);
    if (step == SQLITE_DONE)
        return {};
    if (step != SQLITE_ROW)
        throw std::runtime_error(sqlite3_errmsg(db_));
    auto str = [&](int n) {
        return std::string(reinterpret_cast<const char *>(sqlite3_column_text(q.value, n)));
    };
    return VersionRecord{str(0), str(1),
                         static_cast<std::uint64_t>(sqlite3_column_int64(q.value, 2)),
                         static_cast<std::uint32_t>(sqlite3_column_int64(q.value, 3)), str(4)};
}
void SqliteStore::registerVersion(const VersionRecord &v) {
    Statement q(db_, "INSERT INTO versions VALUES(?,?,?,?,?)");
    bindText(q.value, 1, v.id);
    bindText(q.value, 2, v.path);
    sqlite3_bind_int64(q.value, 3, v.size);
    sqlite3_bind_int64(q.value, 4, v.chunkSize);
    bindText(q.value, 5, v.sha256);
    done(db_, q.value);
}
bool SqliteStore::compareAndSetCurrent(std::string_view path,
                                       const std::optional<std::string> &expected,
                                       std::string_view replacement) {
    if (sqlite3_get_autocommit(db_))
        throw std::runtime_error("CAS requires caller transaction");
    if (expected) {
        Statement q(db_, "UPDATE current_files SET version_id=? WHERE path=? AND version_id=?");
        bindText(q.value, 1, replacement);
        bindText(q.value, 2, path);
        bindText(q.value, 3, *expected);
        done(db_, q.value);
    } else {
        Statement q(db_, "INSERT INTO current_files(path,version_id) SELECT ?,? WHERE NOT "
                         "EXISTS(SELECT 1 FROM current_files WHERE path=?)");
        bindText(q.value, 1, path);
        bindText(q.value, 2, replacement);
        bindText(q.value, 3, path);
        done(db_, q.value);
    }
    return sqlite3_changes(db_) == 1;
}
#include "SqliteSnapshot.inc"
} // namespace chunkharbor
