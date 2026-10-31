#pragma once
#include <string_view>
#include <cstdint>
#include <string>
#include <vector>
struct sqlite3;
namespace chunkharbor {
// Open and migrate a private database. Construction is blocking: call on a disk worker.
class SqliteStore {
  public:
    SqliteStore(std::string_view databasePath, std::string_view schemaPath);
    ~SqliteStore();
    SqliteStore(const SqliteStore &) = delete;
    SqliteStore &operator=(const SqliteStore &) = delete;
    struct Transfer {
        std::string sessionId, logicalPath, expectedSha256, state;
        std::int64_t totalSize = 0, chunkSize = 0;
        std::int64_t mtimeNs = 0;
        std::int64_t updatedAtNs = 0;
        std::string tempPath;
    };
    struct Chunk { std::int64_t index; std::string sha256; };
    void createTransfer(const Transfer &transfer);
    void confirmChunk(std::string_view sessionId, std::int64_t index,
                      std::string_view sha256, std::int64_t durableAtNs);
    std::vector<std::int64_t> confirmedChunks(std::string_view sessionId) const;
    std::vector<Transfer> transfers() const;
    std::vector<Chunk> chunks(std::string_view sessionId) const;
    void setState(std::string_view sessionId, std::string_view from, std::string_view to);
    void forgetChunk(std::string_view sessionId, std::int64_t index);

  private:
    sqlite3 *db_ = nullptr;
};
} // namespace chunkharbor
