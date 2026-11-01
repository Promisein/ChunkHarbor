#pragma once
#include "chunkharbor/protocol/DeltaControl.h"
#include "chunkharbor/protocol/ManifestPage.h"
#include "chunkharbor/protocol/PlanPage.h"
#include "chunkharbor/storage/UploadStorage.h"
#include <variant>
namespace chunkharbor {
class DeltaStorage {
  public:
    explicit DeltaStorage(UploadStorage &);
    ~DeltaStorage();
    protocol::DeltaReady begin(const protocol::BeginDelta &);
    protocol::ManifestAck appendManifest(const protocol::ManifestPage &);
    protocol::ManifestAck finishManifest(const protocol::ManifestDone &);
    using PlanPageOrDone = std::variant<protocol::PlanPage, protocol::PlanDone>;
    PlanPageOrDone planPage(const protocol::GetPlanPage &);
    protocol::DeltaStatus query(const protocol::QuerySession &);
    protocol::ChunkAck write(const protocol::DataChunk &);
    protocol::DeltaResult commit(const protocol::CommitUpload &);
    bool contains(const protocol::SessionId &);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace chunkharbor
