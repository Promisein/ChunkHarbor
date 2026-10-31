#include "chunkharbor/protocol/ChunkAck.h"
#include <iostream>
#include <limits>

using namespace chunkharbor::protocol;
void check(bool ok, const char* name) { if (!ok) throw std::runtime_error(name); }
template<class F> void rejects(F fn) {
    try { fn(); } catch (const ChunkAckError&) { return; }
    throw std::runtime_error("expected ChunkAckError");
}
const std::string id = "0123456789abcdef0123456789abcdef";
const std::string hash = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
std::string body(std::string offset = "0", std::string length = "3", std::string durable = "true",
                 std::string session = "\"" + id + "\"", std::string digest = "\"" + hash + "\"") {
    return "{\"session_id\":" + session + ",\"offset\":" + offset + ",\"data_length\":" + length +
           ",\"chunk_sha256\":" + digest + ",\"durable\":" + durable + "}";
}
int main() {
    try {
        ChunkAck value;
        value.sessionId = {1,0x23,0x45,0x67,0x89,0xab,0xcd,0xef,1,0x23,0x45,0x67,0x89,0xab,0xcd,0xef};
        value.sha256 = {0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
                        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
        value.dataLength = 3;
        const auto golden = "{\"chunk_sha256\":\"" + hash + "\",\"data_length\":3,\"durable\":true,\"offset\":0,\"session_id\":\"" + id + "\"}";
        check(encodeChunkAck(value) == golden, "canonical durable ACK");
        auto parsed = decodeChunkAck(golden);
        check(parsed.sessionId == value.sessionId && parsed.sha256 == value.sha256 && parsed.offset == 0 && parsed.dataLength == 3,
              "golden decoded bytes");
        for (const auto* flag : {"false", "0", "1", "1.0", "\"true\"", "null", "[]", "{}"})
            rejects([&] { decodeChunkAck(body("0","3",flag)); });
        for (const auto* offset : {"-1", "1.0", "1e3", "true", "null", "\"1\"", "18446744073709551616"})
            rejects([&] { decodeChunkAck(body(offset)); });
        for (const auto* length : {"0", "-1", "1048517", "4294967296", "18446744073709551615", "1.0", "1e3", "false", "null", "\"3\""})
            rejects([&] { decodeChunkAck(body("0",length)); });
        for (const auto& text : {std::string{}, std::string(31,'a'), std::string(33,'a'), std::string(32,'A'), std::string(32,'g')})
            rejects([&] { decodeChunkAck(body("0","3","true","\""+text+"\"")); });
        for (const auto& text : {std::string{}, std::string(63,'a'), std::string(65,'a'), std::string(64,'A'), std::string(64,'g')})
            rejects([&] { decodeChunkAck(body("0","3","true","\""+id+"\"","\""+text+"\"")); });
        for (const auto* text : {"null", "true", "1", "[]", "{}", "\"a\\u0000\"", "\"\\ud800\""}) {
            rejects([&] { decodeChunkAck(body("0","3","true",text)); });
            rejects([&] { decodeChunkAck(body("0","3","true","\""+id+"\"",text)); });
        }
        rejects([&] { decodeChunkAck(body("0","3","true","\""+std::string(32,static_cast<char>(255))+"\"")); });
        const auto max = std::numeric_limits<std::uint64_t>::max();
        parsed = decodeChunkAck(body("9007199254740993"));
        check(parsed.offset == 9007199254740993ULL && encodeChunkAck(parsed).find("9007199254740993") != std::string::npos, "uint64 precision");
        value.offset = max-3; check(decodeChunkAck(encodeChunkAck(value)).offset == max-3, "uint64 boundary");
        value.offset = max-2; rejects([&] { encodeChunkAck(value); });
        rejects([&] { decodeChunkAck(body("18446744073709551613")); });
        value.offset = 0;
        for (const auto length : {1U, static_cast<unsigned>(MaxChunkDataSize)}) {
            value.dataLength = length; check(decodeChunkAck(encodeChunkAck(value)).dataLength == length, "length boundary");
        }
        value.dataLength = 0; rejects([&] { encodeChunkAck(value); });
        value.dataLength = static_cast<std::uint32_t>(MaxChunkDataSize+1); rejects([&] { encodeChunkAck(value); });
        value.dataLength = 3;
        DataChunk pending{value.sessionId, 0, value.sha256, "abc"};
        validateChunkAckForChunk(value,pending);
        auto wrong = value; wrong.sessionId[0] ^= 1; rejects([&] { validateChunkAckForChunk(wrong,pending); });
        wrong = value; ++wrong.offset; rejects([&] { validateChunkAckForChunk(wrong,pending); });
        wrong = value; ++wrong.dataLength; rejects([&] { validateChunkAckForChunk(wrong,pending); });
        wrong = value; wrong.sha256[0] ^= 1; rejects([&] { validateChunkAckForChunk(wrong,pending); });
        pending.data.clear(); rejects([&] { validateChunkAckForChunk(value,pending); });
        value.sessionId.fill(0); value.sha256.fill(255);
        parsed = decodeChunkAck(encodeChunkAck(value));
        check(parsed.sessionId == value.sessionId && parsed.sha256 == value.sha256, "opaque bytes preserved");
        const auto valid = body();
        for (const auto& field : {std::string("\"session_id\":\"")+id+"\",", std::string("\"offset\":0,"),
             std::string("\"data_length\":3,"), std::string("\"chunk_sha256\":\"")+hash+"\",", std::string(",\"durable\":true")}) {
            auto missing = valid; const auto pos = missing.find(field); check(pos != std::string::npos,"missing-field fixture");
            missing.erase(pos,field.size()); rejects([&] { decodeChunkAck(missing); });
        }
        for (const auto& invalid : {std::string{}, std::string("{}"), std::string("[]"), valid+valid, valid+"x",
             valid+std::string(1,'\0'), valid+std::string("\0garbage",8), valid+std::string(1,'\0')+valid,
             std::string("\xef\xbb\xbf")+valid, "/*comment*/"+valid, valid.substr(0,valid.size()-1)+",}",
             valid.substr(0,valid.size()-1)+",\"durable\":true}", valid.substr(0,valid.size()-1)+",\"dura\\u0062le\":true}",
             valid.substr(0,valid.size()-1)+",\"extra\":1}"}) rejects([&] { decodeChunkAck(invalid); });
        rejects([] { decodeChunkAck(std::string(1000,'[')+std::string(1000,']')); });
        check(decodeChunkAck(valid+std::string(MaxControlPayload-valid.size(),' ')).dataLength == 3,"64KiB body");
        rejects([&] { decodeChunkAck(valid+std::string(MaxControlPayload-valid.size()+1,' ')); });
        auto wire = encode(Frame{6,max,valid});
        for (std::size_t i=0; i<wire.size(); ++i)
            check(decode(std::string_view(wire).substr(0,i)).status == DecodeStatus::NeedMore,"frame split");
        auto frame = decode(wire+wire);
        check(frame.status == DecodeStatus::Complete && frame.consumed == wire.size() && frame.frame.requestId == max &&
              decodeChunkAck(frame.frame.payload).sha256[0] == 0xba,"frame composition");
        std::cout << "CHUNK_ACK strict durable schema, bounds and correlation passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
