#include "chunkharbor/protocol/BeginUpload.h"
#include <iostream>
#include <limits>

using namespace chunkharbor::protocol;
void check(bool ok, const char* name) { if (!ok) throw std::runtime_error(name); }
template<class F> void rejects(F fn) {
    try { fn(); } catch (const BeginUploadError&) { return; }
    throw std::runtime_error("expected BeginUploadError");
}
const std::string abcHash = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
std::string body(std::string path = "\"a.bin\"", std::string size = "3",
                 std::string mtime = "0", std::string chunk = "262144",
                 std::string hash = "\"" + abcHash + "\"") {
    return "{\"path\":" + path + ",\"size\":" + size + ",\"mtime_ns\":" + mtime +
           ",\"chunk_size\":" + chunk + ",\"sha256\":" + hash + "}";
}
int main() {
    try {
        BeginUpload value{"a.bin", 3, 1730000000000000001LL, 262144, abcHash};
        const std::string golden = "{\"chunk_size\":262144,\"mtime_ns\":1730000000000000001,\"path\":\"a.bin\",\"sha256\":\"" + abcHash + "\",\"size\":3}";
        check(encodeBeginUpload(value) == golden, "canonical golden JSON");
        auto parsed = decodeBeginUpload(golden);
        check(parsed.path == value.path && parsed.size == 3 && parsed.mtimeNs == value.mtimeNs &&
              parsed.chunkSize == 262144 && parsed.sha256 == abcHash, "golden decode");
        for (const auto size : {0ULL, 9007199254740993ULL, 18446744073709551615ULL}) {
            value.size = size;
            check(decodeBeginUpload(encodeBeginUpload(value)).size == size, "uint64 precision");
        }
        for (const auto mtime : {std::numeric_limits<std::int64_t>::min(), std::int64_t{-1},
                                std::int64_t{0}, std::numeric_limits<std::int64_t>::max()}) {
            value.mtimeNs = mtime;
            check(decodeBeginUpload(encodeBeginUpload(value)).mtimeNs == mtime, "int64 precision");
        }
        for (const auto* n : {"-1", "1.0", "1e3", "true", "null", "\"3\"", "[]", "18446744073709551616"})
            rejects([&] { decodeBeginUpload(body("\"a\"", n)); });
        for (const auto* n : {"1.0", "1e3", "false", "null", "\"0\"", "9223372036854775808", "-9223372036854775809"})
            rejects([&] { decodeBeginUpload(body("\"a\"", "3", n)); });
        for (const auto* n : {"0", "-1", "1048517", "4294967296", "1.0", "true", "null", "\"1\""})
            rejects([&] { decodeBeginUpload(body("\"a\"", "3", "0", n)); });
        for (const auto* path : {"", ".", "..", "../a", "a/../b", "a/./b", "/a", "//host/a", "a//b", "a/",
                                 "C:/a", "C:a", "a\\b", "a:stream"}) {
            value.path = path;
            rejects([&] { encodeBeginUpload(value); });
        }
        for (const auto* pathJson : {"null", "3", "true", "[]", "{}", "\"a\\u0000b\"", "\"a\\n\"", "\"a\\u007fb\"", "\"\\u002e\\u002e/a\"", "\"a\\\\b\""})
            rejects([&] { decodeBeginUpload(body(pathJson)); });
        value.path = u8"\u76ee\u5f55/\u4e2d\u6587\U0001f600.bin";
        check(decodeBeginUpload(encodeBeginUpload(value)).path == value.path, "UTF-8 path");
        check(decodeBeginUpload(body("\"\\u76ee\\u5f55/a\"")).path == u8"\u76ee\u5f55/a", "escaped Unicode");
        value.path = std::string(255, 'a'); encodeBeginUpload(value);
        value.path.push_back('a'); rejects([&] { encodeBeginUpload(value); });
        value.path.clear();
        for (int i = 0; i < 16; ++i) value.path += std::string(255, 'a') + '/';
        value.path.back() = 'a'; // 4096 bytes, but final component 256: independently invalid.
        rejects([&] { encodeBeginUpload(value); });
        value.path = std::string(254, 'a');
        for (int i = 0; i < 15; ++i) value.path += '/' + std::string(255, 'a');
        value.path += "/a"; // 4096 bytes, each component within 255.
        check(value.path.size() == 4096, "path boundary fixture");
        check(decodeBeginUpload(encodeBeginUpload(value)).path == value.path, "max path");
        value.path += 'b'; rejects([&] { encodeBeginUpload(value); });
        rejects([&] { decodeBeginUpload(body("\"" + value.path + "\"")); });
        rejects([&] { decodeBeginUpload(body("\"" + std::string(256, 'a') + "\"")); });
        value.path = std::string("a\0b", 3); rejects([&] { encodeBeginUpload(value); });
        for (const std::string& bad : {std::string("\xc0\xaf", 2), std::string("\xed\xa0\x80", 3), std::string("\xf4\x90\x80\x80", 4)}) {
            value.path = bad; rejects([&] { encodeBeginUpload(value); });
            rejects([&] { decodeBeginUpload(body("\"" + bad + "\"")); });
        }
        value.path = "a";
        for (const auto& hash : {std::string{}, std::string(63, 'a'), std::string(65, 'a'), std::string(64, 'g'), std::string(64, 'A')}) {
            value.sha256 = hash; rejects([&] { encodeBeginUpload(value); });
            rejects([&] { decodeBeginUpload(body("\"a\"", "0", "0", "1", "\"" + hash + "\"")); });
        }
        rejects([&] { decodeBeginUpload(body("\"a\"", "0", "0", "1", "null")); });
        value.sha256 = abcHash;
        value.chunkSize = 0; rejects([&] { encodeBeginUpload(value); });
        value.chunkSize = static_cast<std::uint32_t>(MaxChunkDataSize + 1); rejects([&] { encodeBeginUpload(value); });
        for (const auto size : {1U, static_cast<unsigned>(MaxChunkDataSize)}) {
            value.chunkSize = size; check(decodeBeginUpload(encodeBeginUpload(value)).chunkSize == size, "chunk boundary");
        }
        const auto valid = body();
        rejects([&] { decodeBeginUpload(valid + std::string(1, '\0')); });
        rejects([&] { decodeBeginUpload(valid + std::string(1, '\0') + "garbage"); });
        rejects([&] { decodeBeginUpload(valid + std::string(1, '\0') + valid); });
        for (const auto& field : {std::string("\"path\":\"a.bin\","), std::string("\"size\":3,"),
             std::string("\"mtime_ns\":0,"), std::string("\"chunk_size\":262144,"),
             std::string(",\"sha256\":\"") + abcHash + "\""}) {
            auto missing = valid;
            const auto position = missing.find(field);
            check(position != std::string::npos, "missing-field fixture");
            missing.erase(position, field.size());
            rejects([&] { decodeBeginUpload(missing); });
        }
        auto unknown = valid;
        unknown.replace(unknown.find("\"size\""), 6, "\"other\"");
        rejects([&] { decodeBeginUpload(unknown); });
        for (const auto& invalid : {std::string{}, std::string("{}"), std::string("[]"), std::string("null"),
             valid + valid, valid + "x", valid.substr(0, valid.size()-1), std::string("\xef\xbb\xbf") + valid,
             valid.substr(0, valid.size()-1) + ",}", "/*comment*/" + valid,
             valid.substr(0, valid.size()-1) + ",\"size\":3}",
             valid.substr(0, valid.size()-1) + ",\"s\\u0069ze\":3}",
             valid.substr(0, valid.size()-1) + ",\"extra\":0}"})
            rejects([&] { decodeBeginUpload(invalid); });
        rejects([] { decodeBeginUpload(std::string(1000, '[') + std::string(1000, ']')); });
        rejects([&] { decodeBeginUpload(valid.substr(0, valid.size()-1) + ",\"extra\":{\"a\":{}}}"); });
        check(decodeBeginUpload(valid + std::string(MaxControlPayload-valid.size(), ' ')).size == 3, "64KiB body");
        rejects([&] { decodeBeginUpload(valid + std::string(MaxControlPayload-valid.size()+1, ' ')); });
        auto wire = encode(Frame{3, 9007199254740993ULL, valid});
        for (std::size_t i = 0; i < wire.size(); ++i)
            check(decode(std::string_view(wire).substr(0, i)).status == DecodeStatus::NeedMore, "frame split");
        auto frame = decode(wire + wire);
        check(frame.status == DecodeStatus::Complete && frame.consumed == wire.size() &&
              frame.frame.requestId == 9007199254740993ULL && decodeBeginUpload(frame.frame.payload).path == "a.bin", "frame composition");
        std::cout << "BEGIN_UPLOAD schema, integer precision, paths and frame boundaries passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
