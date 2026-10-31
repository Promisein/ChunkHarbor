#include "chunkharbor/protocol/UploadPlan.h"
#include <iostream>
#include <limits>

using namespace chunkharbor::protocol;
void check(bool ok, const char* name) { if (!ok) throw std::runtime_error(name); }
template<class F> void rejects(F fn) {
    try { fn(); } catch (const UploadPlanError&) { return; }
    throw std::runtime_error("expected UploadPlanError");
}
const std::string id = "0123456789abcdef0123456789abcdef";
std::string body(std::string ranges = "[[0,3]]", std::string chunk = "262144",
                 std::string session = "\"" + id + "\"") {
    return "{\"session_id\":" + session + ",\"chunk_size\":" + chunk + ",\"missing_ranges\":" + ranges + "}";
}
int main() {
    try {
        UploadPlan value;
        value.sessionId = {1,0x23,0x45,0x67,0x89,0xab,0xcd,0xef,1,0x23,0x45,0x67,0x89,0xab,0xcd,0xef};
        value.missingRanges = {{0,3}};
        const auto golden = "{\"chunk_size\":262144,\"missing_ranges\":[[0,3]],\"session_id\":\"" + id + "\"}";
        check(encodeUploadPlan(value) == golden, "golden encoding");
        auto parsed = decodeUploadPlan(golden);
        check(parsed.sessionId == value.sessionId && parsed.chunkSize == 262144 &&
              parsed.missingRanges.size() == 1 && parsed.missingRanges[0].begin == 0 && parsed.missingRanges[0].end == 3,
              "golden decoding");
        value.missingRanges = {{0,1},{1,3},{7,9}};
        check(decodeUploadPlan(encodeUploadPlan(value)).missingRanges.size() == 3, "adjacent and sparse ranges");
        for (const auto* ranges : {"null", "{}", "true", "[0,3]", "[[]]", "[[0]]", "[[0,1,2]]", "[[0,0]]", "[[3,2]]",
             "[[0,2],[1,3]]", "[[0,1],[0,1]]", "[[3,4],[0,1]]", "[[0,1],[]]", "[[[0],1]]", "[[{},1]]",
             "[[true,2]]", "[[0,false]]", "[[null,2]]", "[[\"0\",2]]", "[[0,\"2\"]]", "[[-1,2]]",
             "[[0,1.0]]", "[[0,1e3]]", "[[0,18446744073709551616]]"})
            rejects([&] { decodeUploadPlan(body(ranges)); });
        for (const auto* chunk : {"0", "-1", "1048517", "4294967296", "true", "1.0", "1e3", "\"1\"", "null"})
            rejects([&] { decodeUploadPlan(body("[]", chunk)); });
        for (const auto& session : {std::string{}, std::string(31,'a'), std::string(33,'a'), std::string(32,'A'), std::string(32,'g')})
            rejects([&] { decodeUploadPlan(body("[]", "1", "\"" + session + "\"")); });
        for (const auto* session : {"null", "[]", "{}", "1", "true", "\"a\\u0000\""})
            rejects([&] { decodeUploadPlan(body("[]", "1", session)); });
        rejects([&] { decodeUploadPlan(body("[]", "1", "\"" + std::string(32, static_cast<char>(255)) + "\"")); });
        rejects([&] { decodeUploadPlan(body("[]", "1", "\"\\ud800\"")); });
        value.sessionId.fill(255);
        check(decodeUploadPlan(encodeUploadPlan(value)).sessionId == value.sessionId, "high session bytes");
        value.sessionId.fill(0);
        check(decodeUploadPlan(encodeUploadPlan(value)).sessionId == value.sessionId, "opaque zero session");
        const auto max = std::numeric_limits<std::uint64_t>::max();
        auto huge = decodeUploadPlan(body("[[9007199254740993,18446744073709551615]]", "1"));
        check(huge.missingRanges[0].begin == 9007199254740993ULL && huge.missingRanges[0].end == max, "uint64 precision");
        validateUploadPlanForFile(huge, max, 1);
        check(encodeUploadPlan(huge).find("9007199254740993,18446744073709551615") != std::string::npos, "integer encoding");
        auto nearMax = decodeUploadPlan(body("[[0,9223372036854775808]]", "2"));
        validateUploadPlanForFile(nearMax, max, 2);
        rejects([&] { validateUploadPlanForFile(nearMax, max-1, 2); });
        value.chunkSize = 4; value.missingRanges = {{0,3}};
        validateUploadPlanForFile(value, 10, 4);
        validateUploadPlanForFile(value, 12, 4);
        rejects([&] { validateUploadPlanForFile(value, 8, 4); });
        rejects([&] { validateUploadPlanForFile(value, 10, 5); });
        rejects([&] { validateUploadPlanForFile(value, 10, 0); });
        rejects([&] { validateUploadPlanForFile(value, 0, 4); });
        value.missingRanges = {{0,1}}; validateUploadPlanForFile(value, 1, 4);
        value.missingRanges.clear(); validateUploadPlanForFile(value, 0, 4); validateUploadPlanForFile(value, 10, 4);
        check(decodeUploadPlan(encodeUploadPlan(value)).missingRanges.empty(), "empty plan");
        value.missingRanges = {{2,1}}; rejects([&] { encodeUploadPlan(value); });
        rejects([&] { validateUploadPlanForFile(value, 10, 4); });
        value.missingRanges = {{0,2},{1,3}}; rejects([&] { encodeUploadPlan(value); });
        value.missingRanges.clear(); value.chunkSize = 0; rejects([&] { encodeUploadPlan(value); });
        value.chunkSize = static_cast<std::uint32_t>(MaxChunkDataSize+1); rejects([&] { encodeUploadPlan(value); });
        for (const auto chunk : {1U, static_cast<unsigned>(MaxChunkDataSize)}) {
            value.chunkSize = chunk;
            check(decodeUploadPlan(encodeUploadPlan(value)).chunkSize == chunk, "chunk size boundary");
        }
        value.missingRanges.clear();
        std::string ranges = "[";
        for (std::uint64_t i = 0; i < MaxUploadPlanRanges; ++i) {
            value.missingRanges.push_back({i*2,i*2+1});
            ranges += (i ? "," : "") + ("[" + std::to_string(i*2) + "," + std::to_string(i*2+1) + "]");
        }
        ranges += ']';
        check(decodeUploadPlan(body(ranges)).missingRanges.size() == MaxUploadPlanRanges, "max decoded ranges");
        check(decodeUploadPlan(encodeUploadPlan(value)).missingRanges.size() == MaxUploadPlanRanges, "max encoded ranges");
        value.missingRanges.push_back({1024,1025}); rejects([&] { encodeUploadPlan(value); });
        ranges.pop_back(); ranges += ",[1024,1025]]"; rejects([&] { decodeUploadPlan(body(ranges)); });
        const auto valid = body();
        for (const auto& invalid : {std::string{}, std::string("{}"), std::string("[]"), valid + valid, valid + "x",
             valid + std::string(1,'\0'), valid + std::string("\0garbage",8), valid + std::string(1,'\0') + valid,
             std::string("\xef\xbb\xbf") + valid, "/*comment*/" + valid, valid.substr(0,valid.size()-1) + ",}",
             valid.substr(0,valid.size()-1) + ",\"chunk_size\":1}",
             valid.substr(0,valid.size()-1) + ",\"chunk_\\u0073ize\":1}",
             valid.substr(0,valid.size()-1) + ",\"extra\":1}"}) rejects([&] { decodeUploadPlan(invalid); });
        for (const auto& missing : {std::string("{\"chunk_size\":1,\"missing_ranges\":[]}"),
             "{\"session_id\":\"" + id + "\",\"missing_ranges\":[]}",
             "{\"session_id\":\"" + id + "\",\"chunk_size\":1}"})
            rejects([&] { decodeUploadPlan(missing); });
        auto unknown = valid;
        unknown.replace(unknown.find("\"chunk_size\""), 12, "\"other\"");
        rejects([&] { decodeUploadPlan(unknown); });
        rejects([] { decodeUploadPlan(std::string(1000,'[')+std::string(1000,']')); });
        check(decodeUploadPlan(valid + std::string(MaxControlPayload-valid.size(),' ')).missingRanges.size() == 1, "64KiB boundary");
        rejects([&] { decodeUploadPlan(valid + std::string(MaxControlPayload-valid.size()+1,' ')); });
        const auto wire = encode(Frame{4,max,valid});
        for (std::size_t i = 0; i < wire.size(); ++i)
            check(decode(std::string_view(wire).substr(0,i)).status == DecodeStatus::NeedMore, "frame split");
        auto frame = decode(wire+wire);
        check(frame.status == DecodeStatus::Complete && frame.consumed == wire.size() && frame.frame.requestId == max &&
              decodeUploadPlan(frame.frame.payload).missingRanges[0].end == 3, "frame composition");
        std::cout << "UPLOAD_PLAN golden, strict schema, ranges and geometry passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
