#include "chunkharbor/protocol/BeginUpload.h"
#include <nlohmann/json.hpp>
#include <limits>
#include <set>

namespace chunkharbor::protocol {
namespace {
using Json = nlohmann::json;
[[noreturn]] void malformed() { throw BeginUploadError("Invalid BEGIN_UPLOAD fields"); }
void validate(const BeginUpload& value) {
    if (value.path.empty() || value.path.size() > MaxLogicalPathBytes) malformed();
    for (const unsigned char c : value.path)
        if (c < 32 || c == 127 || c == '\\' || c == ':') malformed();
    std::size_t start = 0;
    for (;;) {
        const auto end = value.path.find('/', start);
        const auto component = std::string_view(value.path).substr(start,
            end == std::string::npos ? value.path.size() - start : end - start);
        if (component.empty() || component.size() > MaxPathComponentBytes || component == "." || component == "..")
            malformed();
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (value.chunkSize == 0 || value.chunkSize > MaxChunkDataSize || value.sha256.size() != 64) malformed();
    for (const unsigned char c : value.sha256)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) malformed();
}
Json parse(std::string_view bytes) {
    if (bytes.empty() || bytes.size() > MaxControlPayload ||
        bytes.find('\0') != std::string_view::npos || static_cast<unsigned char>(bytes.front()) == 0xef)
        malformed();
    std::set<std::string> keys;
    const auto callback = [&](int depth, Json::parse_event_t event, Json& parsed) {
        if (depth > 1 || event == Json::parse_event_t::array_start ||
            (event == Json::parse_event_t::object_start && depth != 0)) malformed();
        if (event == Json::parse_event_t::key && !keys.insert(parsed.get<std::string>()).second) malformed();
        return true;
    };
    // Strict UTF-8, no comments/trailing data. Limit depth before building nested DOMs.
    return Json::parse(bytes.begin(), bytes.end(), callback, true, false);
}
std::uint64_t unsignedInteger(const Json& value) {
    if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<std::int64_t>() < 0))
        malformed();
    return value.get<std::uint64_t>();
}
std::int64_t signedInteger(const Json& value) {
    if (!value.is_number_integer()) malformed();
    if (value.is_number_unsigned() && value.get<std::uint64_t>() >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) malformed();
    return value.get<std::int64_t>();
}
} // namespace
std::string encodeBeginUpload(const BeginUpload& value) {
    validate(value);
    try {
        auto bytes = Json{{"path", value.path}, {"size", value.size}, {"mtime_ns", value.mtimeNs},
                          {"chunk_size", value.chunkSize}, {"sha256", value.sha256}}.dump();
        if (bytes.size() > MaxControlPayload) malformed();
        return bytes;
    } catch (const Json::exception&) { malformed(); }
}
BeginUpload decodeBeginUpload(std::string_view payload) {
    try {
        const auto object = parse(payload);
        if (!object.is_object() || object.size() != 5) malformed();
        for (const auto* field : {"path", "size", "mtime_ns", "chunk_size", "sha256"})
            if (!object.contains(field)) malformed();
        if (!object.at("path").is_string() || !object.at("sha256").is_string()) malformed();
        const auto chunkSize = unsignedInteger(object.at("chunk_size"));
        if (chunkSize == 0 || chunkSize > MaxChunkDataSize) malformed();
        BeginUpload value{object.at("path").get<std::string>(), unsignedInteger(object.at("size")),
                          signedInteger(object.at("mtime_ns")), static_cast<std::uint32_t>(chunkSize),
                          object.at("sha256").get<std::string>()};
        validate(value);
        return value;
    } catch (const Json::exception&) { malformed(); }
}
} // namespace chunkharbor::protocol
