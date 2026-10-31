#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <string>
#include <string_view>

namespace chunkharbor {
using Sha256Digest = std::array<std::uint8_t, 32>;

// One owner/thread per context. finish() is terminal; errors are terminal too.
class Sha256 {
public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    void update(std::string_view bytes);
    Sha256Digest finish();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Hash exactly these bytes (including NUL); no ownership or input copy.
Sha256Digest sha256(std::string_view bytes);
std::string toHex(const Sha256Digest& digest);
// Synchronous, fixed 64 KiB read buffer. Call from a worker, not EventLoop/UI.
// Stream is consumed from its current position to EOF; path opens binary input.
Sha256Digest sha256Stream(std::istream& input);
Sha256Digest sha256File(const std::filesystem::path& path);
} // namespace chunkharbor
