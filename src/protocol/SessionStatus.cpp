#include "chunkharbor/protocol/SessionStatus.h"
#include <nlohmann/json.hpp>
#include <set>
namespace chunkharbor::protocol {
namespace {
using J = nlohmann::json;
[[noreturn]] void bad() { throw UploadPlanError("Invalid SESSION_STATUS"); }
int digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    bad();
}
std::string hex(const std::uint8_t *bytes, std::size_t count) {
    std::string out;
    out.reserve(count * 2);
    for (std::size_t i = 0; i < count; ++i) {
        out += "0123456789abcdef"[bytes[i] >> 4];
        out += "0123456789abcdef"[bytes[i] & 15];
    }
    return out;
}
void validate(const SessionStatus &s) {
    if (!s.chunkSize || s.chunkSize > MaxChunkDataSize || s.blockCount > 65536 ||
        s.missing.size() != (static_cast<std::size_t>(s.blockCount) + 7) / 8)
        bad();
    if (s.blockCount % 8 && !s.missing.empty() &&
        (s.missing.back() >> (s.blockCount % 8))) bad();
}
std::uint32_t number(const J &v, std::uint32_t max) {
    if (!v.is_number_integer() || (!v.is_number_unsigned() && v.get<std::int64_t>() < 0)) bad();
    auto n = v.get<std::uint64_t>();
    if (n > max) bad();
    return static_cast<std::uint32_t>(n);
}
} // namespace
std::string encodeSessionStatus(const SessionStatus &s) {
    validate(s);
    auto payload = J{{"session_id", hex(s.sessionId.data(), s.sessionId.size())},
                     {"chunk_size", s.chunkSize}, {"block_count", s.blockCount},
                     {"missing_bitmap", hex(s.missing.data(), s.missing.size())}}.dump();
    if (payload.size() > MaxControlPayload) bad();
    return payload;
}
SessionStatus decodeSessionStatus(std::string_view payload) {
    try {
        if (payload.empty() || payload.size() > MaxControlPayload ||
            payload.find('\0') != std::string_view::npos) bad();
        std::set<std::string> keys;
        auto callback = [&](int depth, J::parse_event_t event, J &value) {
            if (depth > 1 || event == J::parse_event_t::array_start ||
                (event == J::parse_event_t::object_start && depth != 0)) bad();
            if (event == J::parse_event_t::key) {
                auto key = value.get<std::string>();
                if (key != "session_id" && key != "chunk_size" && key != "block_count" &&
                    key != "missing_bitmap") bad();
                if (!keys.insert(key).second) bad();
            }
            return true;
        };
        auto value = J::parse(payload.begin(), payload.end(), callback, true, false);
        if (!value.is_object() || value.size() != 4 || keys.size() != 4 ||
            !value.at("session_id").is_string() || !value.at("missing_bitmap").is_string()) bad();
        const auto id = value.at("session_id").get<std::string>();
        const auto bitmap = value.at("missing_bitmap").get<std::string>();
        SessionStatus s;
        s.chunkSize = number(value.at("chunk_size"), MaxChunkDataSize);
        s.blockCount = number(value.at("block_count"), 65536);
        if (id.size() != 32 || bitmap.size() != 2 * ((s.blockCount + 7) / 8)) bad();
        for (std::size_t i = 0; i < s.sessionId.size(); ++i)
            s.sessionId[i] = static_cast<std::uint8_t>((digit(id[2*i]) << 4) | digit(id[2*i+1]));
        s.missing.resize(bitmap.size() / 2);
        for (std::size_t i = 0; i < s.missing.size(); ++i)
            s.missing[i] = static_cast<std::uint8_t>((digit(bitmap[2*i]) << 4) | digit(bitmap[2*i+1]));
        validate(s);
        return s;
    } catch (const J::exception &) { bad(); }
}
void validateSessionStatusForFile(const SessionStatus &s, std::uint64_t size,
                                  std::uint32_t chunkSize) {
    validate(s);
    if (!chunkSize || s.chunkSize != chunkSize ||
        s.blockCount != size / chunkSize + (size % chunkSize != 0)) bad();
}
} // namespace chunkharbor::protocol
