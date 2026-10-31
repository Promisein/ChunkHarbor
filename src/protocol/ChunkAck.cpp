#include "chunkharbor/protocol/ChunkAck.h"
#include <nlohmann/json.hpp>
#include <limits>
#include <set>

namespace chunkharbor::protocol {
namespace {
using Json = nlohmann::json;
[[noreturn]] void malformed() { throw ChunkAckError("Invalid CHUNK_ACK fields"); }
void validate(const ChunkAck& value) {
    if (value.dataLength == 0 || value.dataLength > MaxChunkDataSize ||
        value.offset > std::numeric_limits<std::uint64_t>::max() - value.dataLength) malformed();
}
std::uint64_t integer(const Json& value) {
    if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<std::int64_t>() < 0)) malformed();
    return value.get<std::uint64_t>();
}
std::uint8_t digit(char c) {
    if (c >= '0' && c <= '9') return static_cast<std::uint8_t>(c-'0');
    if (c >= 'a' && c <= 'f') return static_cast<std::uint8_t>(c-'a'+10);
    malformed();
}
template<std::size_t N> std::string hex(const std::array<std::uint8_t,N>& bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string text(N*2,'0');
    for (std::size_t i=0; i<N; ++i) {
        text[i*2] = digits[bytes[i] >> 4];
        text[i*2+1] = digits[bytes[i] & 15];
    }
    return text;
}
template<std::size_t N> std::array<std::uint8_t,N> unhex(const Json& value) {
    if (!value.is_string()) malformed();
    const auto& text = value.get_ref<const std::string&>();
    if (text.size() != N*2) malformed();
    std::array<std::uint8_t,N> bytes{};
    for (std::size_t i=0; i<N; ++i)
        bytes[i] = static_cast<std::uint8_t>((digit(text[i*2]) << 4) | digit(text[i*2+1]));
    return bytes;
}
Json parse(std::string_view bytes) {
    if (bytes.empty() || bytes.size() > MaxControlPayload || bytes.find('\0') != std::string_view::npos ||
        static_cast<unsigned char>(bytes.front()) == 0xef) malformed();
    std::set<std::string> keys;
    const auto callback = [&](int depth, Json::parse_event_t event, Json& parsed) {
        if (depth > 1 || event == Json::parse_event_t::array_start ||
            (event == Json::parse_event_t::object_start && depth != 0)) malformed();
        if (event == Json::parse_event_t::key) {
            const auto key = parsed.get<std::string>();
            if (key != "session_id" && key != "offset" && key != "data_length" &&
                key != "chunk_sha256" && key != "durable") malformed();
            if (!keys.insert(key).second) malformed();
        }
        return true;
    };
    return Json::parse(bytes.begin(),bytes.end(),callback,true,false);
}
} // namespace
std::string encodeChunkAck(const ChunkAck& value) {
    validate(value);
    return Json{{"session_id",hex(value.sessionId)}, {"offset",value.offset}, {"data_length",value.dataLength},
                {"chunk_sha256",hex(value.sha256)}, {"durable",true}}.dump();
}
ChunkAck decodeChunkAck(std::string_view payload) {
    try {
        const auto object = parse(payload);
        if (!object.is_object() || object.size() != 5) malformed();
        for (const auto* key : {"session_id","offset","data_length","chunk_sha256","durable"})
            if (!object.contains(key)) malformed();
        const auto& durable = object.at("durable");
        if (!durable.is_boolean() || !durable.get<bool>()) malformed();
        const auto length = integer(object.at("data_length"));
        if (length == 0 || length > MaxChunkDataSize) malformed();
        ChunkAck value{unhex<16>(object.at("session_id")), integer(object.at("offset")),
                       static_cast<std::uint32_t>(length), unhex<32>(object.at("chunk_sha256"))};
        validate(value);
        return value;
    } catch (const Json::exception&) { malformed(); }
}
void validateChunkAckForChunk(const ChunkAck& ack, const DataChunk& pending) {
    validate(ack);
    if (ack.sessionId != pending.sessionId || ack.offset != pending.offset ||
        ack.dataLength != pending.data.size() || ack.sha256 != pending.sha256) malformed();
}
} // namespace chunkharbor::protocol
