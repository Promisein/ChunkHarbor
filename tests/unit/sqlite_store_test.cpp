#include "chunkharbor/storage/SqliteStore.h"
#include <sqlite3.h>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <unistd.h>
void check(bool value) { if (!value) throw std::runtime_error("SQLite store test failed"); }
int main(int argc, char **argv) {
    check(argc == 2);
    char name[] = "/tmp/chunkharbor-sqlite-XXXXXX";
    auto *dir = ::mkdtemp(name);
    check(dir != nullptr);
    const auto dbPath = (std::filesystem::path(dir) / "metadata.sqlite3").string();
    {
        chunkharbor::SqliteStore db(dbPath, argv[1]);
        db.createTransfer({"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "docs/a", std::string(64, 'b'), "CREATED", 4, 4, 0, 0, ".sessions/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/data"});
        const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        db.confirmChunk("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 0, std::string(64, 'c'), now);
        check(db.confirmedChunks("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") == std::vector<std::int64_t>{0});
    }
    {
        chunkharbor::SqliteStore db(dbPath, argv[1]);
        check(db.confirmedChunks("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") == std::vector<std::int64_t>{0});
        bool duplicate = false;
        try { db.confirmChunk("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 0, std::string(64, 'c'), 2); }
        catch (const std::runtime_error &) { duplicate = true; }
        check(duplicate);
        check(db.confirmedChunks("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") == std::vector<std::int64_t>{0});
    }
    sqlite3 *raw = nullptr;
    check(sqlite3_open(dbPath.c_str(), &raw) == SQLITE_OK);
    check(sqlite3_exec(raw, "PRAGMA user_version=99", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(raw);
    bool rejected = false;
    try { chunkharbor::SqliteStore db(dbPath, argv[1]); }
    catch (const std::runtime_error &) { rejected = true; }
    check(rejected);
    std::filesystem::remove_all(dir);
}
