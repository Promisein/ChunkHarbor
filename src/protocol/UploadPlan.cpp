#include "chunkharbor/protocol/UploadPlan.h"
#include <nlohmann/json.hpp>
#include <set>

namespace chunkharbor::protocol {
namespace {
using Json = nlohmann::json;
[[noreturn]] void malformed() { throw UploadPlanError("Invalid UPLOAD_PLAN fields"); }
void validate(const UploadPlan& value) {
    if (value.chunkSize == 0 || value.chunkSize > MaxChunkDataSize ||
        value.missingRanges.size() > MaxUploadPlanRanges) malformed();
    std::uint64_t previousEnd = 0;
    for (const auto& range : value.missingRanges) {
        if (range.begin >= range.end || range.begin < previousEnd) malformed();
        previousEnd = range.end;
    }
}
std::uint64_t integer(const Json& value) {
    if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<std::int64_t>() < 0))
        malformed();
    return value.get<std::uint64_t>();
}
std::uint8_t hexDigit(char c) {
    if (c >= '0' && c <= '9') return static_cast<std::uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<std::uint8_t>(c - 'a' + 10);
    malformed();
}
Json parse(std::string_view bytes) {
    if (bytes.empty() || bytes.size() > MaxControlPayload || bytes.find('\0') != std::string_view::npos ||
        static_cast<unsigned char>(bytes.front()) == 0xef) malformed();
    std::set<std::string> keys;
    std::size_t pairs = 0;
    const auto callback = [&](int depth, Json::parse_event_t event, Json& parsed) {
        if (depth > 3 || (event == Json::parse_event_t::object_start && depth != 0)) malformed();
        if (event == Json::parse_event_t::array_start) {
            if (depth == 0 || depth > 2) malformed();
            // Reject excess pairs before the DOM accumulates an unbounded list.
            if (depth == 2 && ++pairs > MaxUploadPlanRanges) malformed();
        }
        if (event == Json::parse_event_t::key) {
            const auto key = parsed.get<std::string>();
            if (key != "session_id" && key != "chunk_size" && key != "missing_ranges") malformed();
            if (!keys.insert(key).second) malformed();
        }
        return true;
    };
    return Json::parse(bytes.begin(), bytes.end(), callback, true, false);
}
} // namespace
std::string encodeUploadPlan(const UploadPlan& value) {
    validate(value);
    constexpr char digits[] = "0123456789abcdef";
    std::string session(32, '0');
    for (std::size_t i = 0; i < value.sessionId.size(); ++i) {
        session[i*2] = digits[value.sessionId[i] >> 4];
        session[i*2+1] = digits[value.sessionId[i] & 15];
    }
    Json ranges = Json::array();
    for (const auto& range : value.missingRanges) ranges.push_back(Json::array({range.begin,range.end}));
    auto bytes = Json{{"session_id", session}, {"chunk_size", value.chunkSize}, {"missing_ranges", ranges}}.dump();
    if (bytes.size() > MaxControlPayload) malformed();
    return bytes;
}
UploadPlan decodeUploadPlan(std::string_view payload) {
    try {
        const auto object = parse(payload);
        if (!object.is_object() || object.size() != 3) malformed();
        for (const auto* key : {"session_id", "chunk_size", "missing_ranges"})
            if (!object.contains(key)) malformed();
        if (!object.at("session_id").is_string()) malformed();
        const auto session = object.at("session_id").get<std::string>();
        if (session.size() != 32) malformed();
        UploadPlan value;
        for (std::size_t i = 0; i < value.sessionId.size(); ++i)
            value.sessionId[i] = static_cast<std::uint8_t>((hexDigit(session[i*2]) << 4) | hexDigit(session[i*2+1]));
        const auto chunkSize = integer(object.at("chunk_size"));
        if (chunkSize == 0 || chunkSize > MaxChunkDataSize) malformed();
        value.chunkSize = static_cast<std::uint32_t>(chunkSize);
        const auto& ranges = object.at("missing_ranges");
        if (!ranges.is_array() || ranges.size() > MaxUploadPlanRanges) malformed();
        value.missingRanges.reserve(ranges.size());
        for (const auto& range : ranges) {
            if (!range.is_array() || range.size() != 2) malformed();
            value.missingRanges.push_back({integer(range[0]), integer(range[1])});
        }
        validate(value);
        return value;
    } catch (const Json::exception&) { malformed(); }
}
void validateUploadPlanForFile(const UploadPlan& value, std::uint64_t fileSize, std::uint32_t requestedChunkSize) {
    validate(value);
    if (requestedChunkSize != value.chunkSize) malformed();
    // Ceiling division without overflowing fileSize + chunkSize - 1.
    const auto blocks = fileSize / value.chunkSize + (fileSize % value.chunkSize != 0);
    for (const auto& range : value.missingRanges) if (range.end > blocks) malformed();
}
} // namespace chunkharbor::protocol
