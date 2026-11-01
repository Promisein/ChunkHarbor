#pragma once
#include "chunkharbor/protocol/DeltaControl.h"
#include "chunkharbor/protocol/ManifestPage.h"
#include <functional>
#include <optional>
namespace chunkharbor::client {
using Digest = std::array<std::uint8_t, 32>;
struct SnapshotIdentity {
    std::uint64_t size = 0;
    std::int64_t mtimeNs = 0;
    Digest sha256{};
};
// Implementations retain one opened file; reads must be exact or throw.
struct SourceSnapshot {
    protocol::BeginUpload metadata;
    std::function<std::string(std::uint64_t, std::size_t)> read;
    std::function<Digest(std::string_view)> hash;
    std::function<void()> verify;
};
class ManifestReader {
    SourceSnapshot &source_;
    protocol::SessionId session_;
    std::uint32_t page_ = 0;

  public:
    ManifestReader(SourceSnapshot &source, protocol::SessionId session)
        : source_(source), session_(session) {}
    std::optional<protocol::ManifestPage> next();
};
} // namespace chunkharbor::client
