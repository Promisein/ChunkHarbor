#include "chunkharbor/storage/SqliteStore.h"
#include <sqlite3.h>
#include <fstream>
#include <stdexcept>
#include <string>
#include <chrono>
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
                            ? reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0)) : "";
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
    if (sqlite3_bind_text(stmt, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK)
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
    if (sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                         SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
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
            if (integer(db_, "SELECT count(*) FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'") != 0)
                throw std::runtime_error("unversioned database is not empty");
            std::ifstream input{std::string(schemaPath)};
            if (!input)
                throw std::runtime_error("schema file unavailable");
            std::string schema((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            exec(db_, schema.c_str());
            if (integer(db_, "PRAGMA user_version") != 1)
                throw std::runtime_error("schema migration did not set version");
        } else if (version != 1 || integer(db_, "SELECT version FROM schema_version") != 1) {
            throw std::runtime_error("unsupported SQLite schema version");
        }
    } catch (...) {
        sqlite3_close(db_);
        db_ = nullptr;
        throw;
    }
}
SqliteStore::~SqliteStore() { sqlite3_close(db_); }
void SqliteStore::createTransfer(const Transfer &t) {
    Statement stmt(db_, "INSERT INTO transfers(session_id,logical_path,expected_sha256,total_size,chunk_size,mtime_ns,temp_path,state,created_at_ns,updated_at_ns) VALUES (?,?,?,?,?,?,?,'CREATED',?,?)");
    bindText(stmt.value, 1, t.sessionId);
    bindText(stmt.value, 2, t.logicalPath);
    bindText(stmt.value, 3, t.expectedSha256);
    sqlite3_bind_int64(stmt.value, 4, t.totalSize);
    sqlite3_bind_int64(stmt.value, 5, t.chunkSize);
    auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();
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
        Statement chunk(db_, "INSERT INTO transfer_chunks(session_id,chunk_index,sha256,durable_at_ns) VALUES (?,?,?,?)");
        bindText(chunk.value, 1, sessionId);
        sqlite3_bind_int64(chunk.value, 2, index);
        bindText(chunk.value, 3, sha256);
        sqlite3_bind_int64(chunk.value, 4, durableAtNs);
        done(db_, chunk.value);
        Statement transfer(db_, "UPDATE transfers SET state='RECEIVING',updated_at_ns=MAX(updated_at_ns,?) WHERE session_id=? AND state IN ('CREATED','RECEIVING')");
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
    Statement stmt(db_, "SELECT chunk_index FROM transfer_chunks WHERE session_id=? ORDER BY chunk_index");
    bindText(stmt.value, 1, sessionId);
    std::vector<std::int64_t> result;
    for (;;) {
        const int step = sqlite3_step(stmt.value);
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(db_));
        result.push_back(sqlite3_column_int64(stmt.value, 0));
    }
    return result;
}
std::vector<SqliteStore::Transfer> SqliteStore::transfers() const {
    Statement stmt(db_, "SELECT session_id,logical_path,expected_sha256,state,total_size,chunk_size,mtime_ns,temp_path,updated_at_ns FROM transfers");
    std::vector<Transfer> result;
    for (;;) {
        int step = sqlite3_step(stmt.value);
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(db_));
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
    Statement stmt(db_, "SELECT chunk_index,sha256 FROM transfer_chunks WHERE session_id=? ORDER BY chunk_index");
    bindText(stmt.value, 1, sessionId);
    std::vector<Chunk> result;
    for (;;) {
        int step = sqlite3_step(stmt.value);
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(db_));
        result.push_back({sqlite3_column_int64(stmt.value, 0),
                          reinterpret_cast<const char *>(sqlite3_column_text(stmt.value, 1))});
    }
    return result;
}
void SqliteStore::setState(std::string_view sessionId, std::string_view from, std::string_view to) {
    Statement stmt(db_, "UPDATE transfers SET state=?,updated_at_ns=MAX(updated_at_ns,?) WHERE session_id=? AND state=?");
    bindText(stmt.value, 1, to);
    auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();
    sqlite3_bind_int64(stmt.value, 2, now);
    bindText(stmt.value, 3, sessionId);
    bindText(stmt.value, 4, from);
    done(db_, stmt.value);
    if (sqlite3_changes(db_) != 1) throw std::runtime_error("invalid transfer state transition");
}
void SqliteStore::forgetChunk(std::string_view sessionId, std::int64_t index) {
    Statement stmt(db_, "DELETE FROM transfer_chunks WHERE session_id=? AND chunk_index=?");
    bindText(stmt.value, 1, sessionId);
    sqlite3_bind_int64(stmt.value, 2, index);
    done(db_, stmt.value);
}
} // namespace chunkharbor
