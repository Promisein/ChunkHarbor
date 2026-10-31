#pragma once
#include "chunkharbor/protocol/BeginUpload.h"
#include "chunkharbor/protocol/ChunkAck.h"
#include "chunkharbor/protocol/UploadControl.h"
#include "chunkharbor/protocol/UploadPlan.h"
#include "chunkharbor/protocol/SessionStatus.h"
#include <memory>
namespace chunkharbor {
struct UploadSession;
using UploadSessionPtr = std::shared_ptr<UploadSession>;
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

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace chunkharbor
