#pragma once
#include "chunkharbor/protocol/DirectorySnapshot.h"
#include "chunkharbor/protocol/VersionHistory.h"
#include "chunkharbor/protocol/BeginUpload.h"
#include "chunkharbor/protocol/ChunkAck.h"
#include "chunkharbor/protocol/SessionStatus.h"
#include "chunkharbor/protocol/UploadControl.h"
#include "chunkharbor/protocol/UploadPlan.h"
#include "chunkharbor/protocol/FilesList.h"
#include "chunkharbor/protocol/Download.h"
#include <memory>
namespace chunkharbor {
struct UploadSession;
using UploadSessionPtr = std::shared_ptr<UploadSession>;
struct DownloadSession;
using DownloadSessionPtr = std::shared_ptr<DownloadSession>;
struct OpenedDownload {
    DownloadSessionPtr session;
    protocol::DownloadReady ready;
};
struct StartedUpload {
    UploadSessionPtr session;
    protocol::UploadPlan plan;
};
struct QueriedUpload {
    UploadSessionPtr session;
    protocol::SessionStatus status;
    bool committed = false;
    protocol::CommitResult result;
};
// Synchronous disk operations: call only on workers. Store outlives its tasks.
class UploadStorage {
  public:
    explicit UploadStorage(std::string_view absoluteRoot);
    ~UploadStorage();
    UploadStorage(const UploadStorage &) = delete;
    UploadStorage &operator=(const UploadStorage &) = delete;
    StartedUpload begin(const protocol::BeginUpload &);
    QueriedUpload query(const protocol::QuerySession &);
    protocol::ChunkAck write(const UploadSessionPtr &, const protocol::DataChunk &);
    protocol::CommitResult commit(const UploadSessionPtr &, const protocol::CommitUpload &);
    protocol::SnapshotInfo createSnapshot(const std::string &root);
    protocol::SnapshotInfo getSnapshot(const std::string &id);
    protocol::SnapshotsPage listSnapshots(const protocol::ListSnapshots &);
    protocol::SnapshotEntriesPage listSnapshotEntries(const protocol::ListSnapshotEntries &);
    protocol::VersionsResult listVersions(const protocol::ListVersions &);
    protocol::FilesResult listFiles(const protocol::ListFiles &);
    OpenedDownload openDownload(const protocol::BeginDownload &,
        protocol::DownloadSelection = protocol::DownloadSelection::Current);
    protocol::DownloadChunk readDownload(const DownloadSessionPtr &, const protocol::GetDownloadChunk &);

  private:
    friend class DeltaStorage;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace chunkharbor
