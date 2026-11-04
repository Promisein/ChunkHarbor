#include "chunkharbor/protocol/DirectorySnapshot.h"
#include <stdexcept>

using namespace chunkharbor::protocol;

int main() {
    const std::string id(32, 'a');
    std::string prefix;
    for (int i = 0; i < 16; ++i) prefix += std::string(254, '"') + '/';
    SnapshotEntriesPage page{id, {}, {}};
    for (std::uint32_t i = 0; i < MaxSnapshotEntriesPerPage; ++i)
        page.entries.push_back({prefix + std::to_string(i), id, 123, std::string(64, 'b')});
    page.nextAfterPath = page.entries.back().path;
    const auto payload = encodeSnapshotEntriesPage(page);
    const auto decoded = decodeSnapshotEntriesPage(payload);
    if (payload.size() > MaxControlPayload || decoded.entries.size() != page.entries.size() ||
        decoded.nextAfterPath != page.nextAfterPath || decoded.entries.front().path != page.entries.front().path)
        throw std::runtime_error("snapshot entry page exceeds wire budget or loses its cursor");
    const auto info = decodeSnapshotInfo(encodeSnapshotInfo({id, "root", 1, 2, 0}));
    if (info.id != id || info.root != "root" || info.sequence != 1 || info.createdAtSeconds != 2 || info.fileCount != 0)
        throw std::runtime_error("snapshot info did not round trip");
    try {
        decodeSnapshotInfo(R"({"snapshot_id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","root":"root","sequence":1,"created_at_s":2,"file_count":0,"state":"creating"})");
    } catch (const UploadFailure &) {
        return 0;
    }
    throw std::runtime_error("snapshot decoder accepted an uncommitted response");
}
