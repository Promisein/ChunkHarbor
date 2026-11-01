#pragma once
#include "chunkharbor/client_core/SourceSnapshot.h"
#include "chunkharbor/protocol/PlanPage.h"
namespace chunkharbor::client {
struct DeltaCheckpoint {
    protocol::SessionId session{};
    std::optional<protocol::VersionId> base;
};
struct DeltaOutcome {
    protocol::PlanDone plan;
    std::optional<protocol::DeltaResult> result;
    std::uint64_t sentBlocks = 0, acknowledgedBytes = 0;
};
struct DeltaOptions {
    protocol::DeltaMode mode = protocol::DeltaMode::Upload;
    protocol::BaseSelector base;
    std::optional<DeltaCheckpoint> resume;
    std::function<void(const DeltaCheckpoint &)> save;
    std::function<bool()> cancelled = [] { return false; };
};
// Serial synchronous driver, no socket/Qt/POSIX dependency. Request callback owns transport.
DeltaOutcome runDelta(SourceSnapshot &,
                      const std::function<protocol::Frame(const protocol::Frame &)> &,
                      const DeltaOptions &);
} // namespace chunkharbor::client
