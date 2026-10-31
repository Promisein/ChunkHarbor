#include "chunkharbor/protocol/Handshake.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <set>

namespace chunkharbor::protocol {
namespace {
using Json = nlohmann::json;
[[noreturn]] void malformed() { throw MessageError("MALFORMED_MESSAGE", "Invalid handshake fields"); }
Json parse(std::string_view bytes) {
    // The JSON lexer treats raw NUL as end-of-input; inspect the entire body
    // before parsing so a valid prefix cannot hide trailing bytes.
    if (bytes.empty() || bytes.size() > MaxHandshakePayload ||
        bytes.find('\0') != std::string_view::npos || bytes.front() == '\xef') malformed();
    std::set<std::string> keys;
    const auto callback = [&](int depth, Json::parse_event_t event, Json& parsed) {
        if (depth > 3 || (event == Json::parse_event_t::object_start && depth != 0)) malformed();
        if (event == Json::parse_event_t::key && !keys.insert(parsed.get<std::string>()).second) malformed();
        return true;
    };
    try { return Json::parse(bytes.begin(), bytes.end(), callback, true, false); }
    catch (const Json::exception&) { malformed(); }
}
void members(const Json& object, std::initializer_list<const char*> fields) {
    if (!object.is_object() || object.size() != fields.size()) malformed();
    for (const auto* field : fields) if (!object.contains(field)) malformed();
}
void text(const std::string& value, std::size_t max, bool token = false) {
    if (value.empty() || value.size() > max) malformed();
    for (const unsigned char c : value)
        if (token ? !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') : (c < 32 || c > 126))
            malformed();
}
std::string string(const Json& value) {
    if (!value.is_string()) malformed();
    return value.get<std::string>();
}
std::uint64_t integer(const Json& value, std::uint64_t max) {
    if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<std::int64_t>() < 0)) malformed();
    const auto result = value.get<std::uint64_t>();
    if (result == 0 || result > max) malformed();
    return result;
}
void capabilities(const std::vector<std::string>& values) {
    if (values.size() > 16) malformed();
    std::set<std::string> seen;
    for (const auto& value : values) {
        text(value, 32, true);
        if (!seen.insert(value).second) malformed();
    }
}
std::vector<std::string> capabilities(const Json& values) {
    if (!values.is_array() || values.size() > 16) malformed();
    std::vector<std::string> result;
    for (const auto& value : values) result.push_back(string(value));
    capabilities(result); return result;
}
void validate(const Hello& value) {
    text(value.clientName, 64); capabilities(value.capabilities);
    if (value.versions.empty() || value.versions.size() > 8) malformed();
    std::set<std::uint16_t> seen;
    for (const auto version : value.versions) if (version == 0 || !seen.insert(version).second) malformed();
}
void validate(const HelloAck& value) {
    text(value.serverName, 64); capabilities(value.capabilities);
    if (!value.version || !value.maxControlPayload || value.maxControlPayload > MaxControlPayload ||
        !value.maxDataPayload || value.maxDataPayload > MaxDataPayload) malformed();
}
void validate(const HandshakeError& value) {
    const std::vector<std::string> allowed{"MALFORMED_MESSAGE", "UNSUPPORTED_VERSION", "HANDSHAKE_REQUIRED",
                                           "ALREADY_HANDSHAKEN", "UNSUPPORTED_OPERATION"};
    if (std::find(allowed.begin(), allowed.end(), value.code) == allowed.end()) malformed();
    text(value.message, 160);
}
}
std::string encodeHello(const Hello& value) {
    validate(value);
    return Json{{"client_name", value.clientName}, {"versions", value.versions}, {"capabilities", value.capabilities}}.dump();
}
Hello decodeHello(std::string_view payload) {
    const auto object = parse(payload); members(object, {"client_name", "versions", "capabilities"});
    Hello value; value.clientName = string(object.at("client_name")); value.versions.clear();
    const auto& versions = object.at("versions");
    if (!versions.is_array() || versions.empty() || versions.size() > 8) malformed();
    for (const auto& version : versions) value.versions.push_back(static_cast<std::uint16_t>(integer(version, 65535)));
    value.capabilities = capabilities(object.at("capabilities")); validate(value); return value;
}
std::string encodeHelloAck(const HelloAck& value) {
    validate(value);
    return Json{{"version", value.version}, {"server_name", value.serverName}, {"capabilities", value.capabilities},
                {"max_control_payload", value.maxControlPayload}, {"max_data_payload", value.maxDataPayload}}.dump();
}
HelloAck decodeHelloAck(std::string_view payload) {
    const auto object = parse(payload);
    members(object, {"version", "server_name", "capabilities", "max_control_payload", "max_data_payload"});
    HelloAck value; value.version = static_cast<std::uint16_t>(integer(object.at("version"), 65535));
    value.serverName = string(object.at("server_name")); value.capabilities = capabilities(object.at("capabilities"));
    value.maxControlPayload = integer(object.at("max_control_payload"), MaxControlPayload);
    value.maxDataPayload = integer(object.at("max_data_payload"), MaxDataPayload);
    validate(value); return value;
}
std::string encodeHandshakeError(const HandshakeError& value) {
    validate(value); return Json{{"code", value.code}, {"message", value.message}}.dump();
}
HandshakeError decodeHandshakeError(std::string_view payload) {
    const auto object = parse(payload); members(object, {"code", "message"});
    HandshakeError value{string(object.at("code")), string(object.at("message"))}; validate(value); return value;
}
HelloAck negotiateHello(const Hello& offer) {
    validate(offer);
    if (std::find(offer.versions.begin(), offer.versions.end(), 1) == offer.versions.end())
        throw MessageError("UNSUPPORTED_VERSION", "No common protocol version");
    HelloAck ack;
    if (std::find(offer.capabilities.begin(), offer.capabilities.end(), "handshake") != offer.capabilities.end())
        ack.capabilities.push_back("handshake");
    return ack;
}
} // namespace chunkharbor::protocol
