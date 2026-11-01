#include "chunkharbor/protocol/ManifestPage.h"
#include "chunkharbor/protocol/DataChunk.h"
#include <algorithm>
#include <nlohmann/json.hpp>
#include <set>
namespace chunkharbor::protocol {
namespace {
using J = nlohmann::json;
[[noreturn]] void bad() { throw ManifestPageError("Invalid MANIFEST_PAGE"); }
void validate(const ManifestPage &p) {
    if (p.pageIndex >= 128 || p.firstBlockIndex != p.pageIndex * 512 || p.hashes.empty() ||
        p.hashes.size() > MaxManifestPageHashes || p.hashes.size() > 65536 - p.firstBlockIndex)
        bad();
}
template <std::size_t N> std::string hex(const std::array<std::uint8_t, N> &a) {
    std::string s;
    s.reserve(N * 2);
    for (auto b : a) {
        s += "0123456789abcdef"[b >> 4];
        s += "0123456789abcdef"[b & 15];
    }
    return s;
}
int digit(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    bad();
}
template <std::size_t N> std::array<std::uint8_t, N> unhex(const J &value) {
    if (!value.is_string())
        bad();
    const auto &s = value.get_ref<const std::string &>();
    if (s.size() != N * 2)
        bad();
    std::array<std::uint8_t, N> result{};
    for (std::size_t i = 0; i < N; ++i)
        result[i] = static_cast<std::uint8_t>((digit(s[i * 2]) << 4) | digit(s[i * 2 + 1]));
    return result;
}
std::uint32_t number(const J &v) {
    if (!v.is_number_integer() || (!v.is_number_unsigned() && v.get<std::int64_t>() < 0))
        bad();
    auto n = v.get<std::uint64_t>();
    if (n > 65536)
        bad();
    return static_cast<std::uint32_t>(n);
}
} // namespace
std::string encodeManifestPage(const ManifestPage &p) {
    validate(p);
    J hashes = J::array();
    for (const auto &h : p.hashes)
        hashes.push_back(hex(h));
    auto bytes = J{{"session_id", hex(p.sessionId)},
                   {"page_index", p.pageIndex},
                   {"first_block_index", p.firstBlockIndex},
                   {"hashes", std::move(hashes)}}
                     .dump();
    if (bytes.size() > MaxControlPayload)
        bad();
    return bytes;
}
ManifestPage decodeManifestPage(std::string_view bytes) {
    if (bytes.empty() || bytes.size() > MaxControlPayload ||
        bytes.find('\0') != std::string_view::npos ||
        static_cast<unsigned char>(bytes.front()) == 0xef)
        bad();
    try {
        std::set<std::string> keys;
        std::string currentKey;
        std::size_t count = 0;
        auto callback = [&](int depth, J::parse_event_t event, J &v) {
            if (depth > 2 || (event == J::parse_event_t::object_start && depth != 0))
                bad();
            if (event == J::parse_event_t::key) {
                currentKey = v.get<std::string>();
                if (currentKey != "session_id" && currentKey != "page_index" &&
                    currentKey != "first_block_index" && currentKey != "hashes")
                    bad();
                if (!keys.insert(currentKey).second)
                    bad();
            }
            if (event == J::parse_event_t::array_start && (depth != 1 || currentKey != "hashes"))
                bad();
            if (event == J::parse_event_t::value && depth == 2) {
                if (++count > MaxManifestPageHashes)
                    bad();
                (void)unhex<32>(v);
            }
            return true;
        };
        auto j = J::parse(bytes.begin(), bytes.end(), callback, true, false);
        if (!j.is_object() || j.size() != 4 || keys.size() != 4 || !j.at("hashes").is_array())
            bad();
        ManifestPage p;
        p.sessionId = unhex<16>(j.at("session_id"));
        p.pageIndex = number(j.at("page_index"));
        p.firstBlockIndex = number(j.at("first_block_index"));
        for (const auto &h : j.at("hashes"))
            p.hashes.push_back(unhex<32>(h));
        validate(p);
        return p;
    } catch (const J::exception &) {
        bad();
    }
}
void validateManifestPageForFile(const ManifestPage &p, const std::array<std::uint8_t, 16> &id,
                                 std::uint64_t size, std::uint32_t chunkSize,
                                 std::uint32_t pageIndex) {
    validate(p);
    if (!chunkSize || chunkSize > MaxChunkDataSize || size > 64ULL * 1024 * 1024)
        bad();
    auto blocks = size / chunkSize + (size % chunkSize != 0);
    if (blocks > 65536 || p.sessionId != id || p.pageIndex != pageIndex ||
        p.firstBlockIndex >= blocks ||
        p.hashes.size() != std::min<std::uint64_t>(512, blocks - p.firstBlockIndex))
        bad();
}
} // namespace chunkharbor::protocol
