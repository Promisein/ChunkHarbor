#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace chunkharbor::protocol {
constexpr std::size_t MaxManifestPageHashes = 512;
class ManifestPageError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};
struct ManifestPage {
    std::array<std::uint8_t, 16> sessionId{};
    std::uint32_t pageIndex = 0;
    std::uint32_t firstBlockIndex = 0;
    std::vector<std::array<std::uint8_t, 32>> hashes;
};
// Type 18 body only. Owns all decoded data; no hashing, I/O or session state.
std::string encodeManifestPage(const ManifestPage &value);
ManifestPage decodeManifestPage(std::string_view payload);
void validateManifestPageForFile(const ManifestPage &value,
                                 const std::array<std::uint8_t, 16> &expectedSessionId,
                                 std::uint64_t fileSize, std::uint32_t chunkSize,
                                 std::uint32_t expectedPageIndex);
} // namespace chunkharbor::protocol
