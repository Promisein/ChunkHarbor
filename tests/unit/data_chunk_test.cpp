#include "chunkharbor/protocol/DataChunk.h"
#include <iostream>
#include <limits>

using namespace chunkharbor::protocol;
void check(bool ok, const char* name) { if (!ok) throw std::runtime_error(name); }
template<class F> void rejects(F fn) {
    try { fn(); } catch (const DataChunkError&) { return; }
    throw std::runtime_error("expected DataChunkError");
}
void putLength(std::string& bytes, std::uint32_t size) {
    for (unsigned i = 0; i < 4; ++i) bytes[27-i] = static_cast<char>(size >> (8*i));
}
int main() {
    try {
        DataChunk chunk;
        for (std::size_t i = 0; i < chunk.sessionId.size(); ++i) chunk.sessionId[i] = static_cast<std::uint8_t>(i);
        chunk.offset = 0x0102030405060708ULL;
        chunk.sha256 = {0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
                        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
        chunk.data = "abc";
        const unsigned char golden[] = {
            0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,
            1,2,3,4,5,6,7,8, 0,0,0,3,
            0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
            0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad,
            'a','b','c'};
        const std::string expected(reinterpret_cast<const char*>(golden), sizeof(golden));
        check(encodeDataChunk(chunk) == expected, "wire golden vector");
        auto decoded = decodeDataChunk(expected);
        check(decoded.sessionId == chunk.sessionId && decoded.offset == chunk.offset &&
              decoded.sha256 == chunk.sha256 && decoded.data == "abc", "golden decode");
        for (std::size_t n = 0; n < expected.size(); ++n)
            rejects([&] { decodeDataChunk(std::string_view(expected).substr(0, n)); });
        rejects([&] { decodeDataChunk(expected + "x"); });
        rejects([] { decodeDataChunk(std::string(DataChunkHeaderSize, '\0')); });
        for (std::uint32_t n : {0U, 1U, 2U, 4U, 0xffffffffU}) {
            auto bad = expected; putLength(bad, n);
            rejects([&] { decodeDataChunk(bad); });
        }
        chunk.data.clear(); rejects([&] { encodeDataChunk(chunk); });
        chunk.data = std::string("\0\x80\xff", 3);
        auto owned = encodeDataChunk(chunk);
        decoded = decodeDataChunk(owned); owned.assign(owned.size(), 'x');
        check(decoded.data == chunk.data, "owned binary decode");
        chunk.sessionId.fill(255); chunk.sha256.fill(0);
        decoded = decodeDataChunk(encodeDataChunk(chunk));
        check(decoded.sessionId == chunk.sessionId && decoded.sha256 == chunk.sha256,
              "opaque session and digest bytes");
        const auto max = std::numeric_limits<std::uint64_t>::max();
        chunk.offset = max - 3;
        check(decodeDataChunk(encodeDataChunk(chunk)).offset == max-3, "uint64 boundary");
        chunk.offset = max - 2;
        rejects([&] { encodeDataChunk(chunk); });
        auto overflow = expected;
        for (int i = 16; i < 24; ++i) overflow[i] = static_cast<char>(255);
        rejects([&] { decodeDataChunk(overflow); });
        chunk.offset = 0;
        chunk.data.assign(MaxChunkDataSize, 'z');
        auto largest = encodeDataChunk(chunk);
        check(largest.size() == MaxDataPayload && decodeDataChunk(largest).data == chunk.data, "max body");
        validateDataChunkRange(chunk, MaxChunkDataSize, static_cast<std::uint32_t>(MaxChunkDataSize));
        chunk.data.push_back('z'); rejects([&] { encodeDataChunk(chunk); });
        rejects([&] { decodeDataChunk(largest + "x"); });
        auto oversizedLength = largest; putLength(oversizedLength, static_cast<std::uint32_t>(MaxChunkDataSize + 1));
        rejects([&] { decodeDataChunk(oversizedLength); });

        chunk.data = "abcd"; chunk.offset = 0;
        validateDataChunkRange(chunk, 10, 4);
        chunk.offset = 4; validateDataChunkRange(chunk, 10, 4);
        chunk.offset = 8; chunk.data = "ab"; validateDataChunkRange(chunk, 10, 4);
        chunk.offset = 1; rejects([&] { validateDataChunkRange(chunk, 10, 4); });
        chunk.offset = 8; chunk.data = "abc"; rejects([&] { validateDataChunkRange(chunk, 10, 4); });
        chunk.data = "a"; rejects([&] { validateDataChunkRange(chunk, 10, 4); });
        chunk.offset = 0; rejects([&] { validateDataChunkRange(chunk, 10, 4); });
        rejects([&] { validateDataChunkRange(chunk, 0, 4); });
        rejects([&] { validateDataChunkRange(chunk, 10, 0); });
        rejects([&] { validateDataChunkRange(chunk, 10, static_cast<std::uint32_t>(MaxChunkDataSize + 1)); });
        chunk.offset = 10; rejects([&] { validateDataChunkRange(chunk, 10, 4); });
        chunk.offset = max; rejects([&] { validateDataChunkRange(chunk, max, 4); });
        chunk.offset = max-3; chunk.data = "abc"; validateDataChunkRange(chunk, max, 4);
        chunk.offset = 0; chunk.data = "abc";
        DataChunk single; single.data = "x";
        validateDataChunkRange(single, 1, 1);
        single.data.clear(); rejects([&] { validateDataChunkRange(single, 1, 1); });
        auto wire = encode(Frame{5, max, encodeDataChunk(chunk)});
        for (std::size_t i = 0; i < wire.size(); ++i)
            check(decode(std::string_view(wire).substr(0, i)).status == DecodeStatus::NeedMore, "frame split");
        auto first = decode(wire + wire);
        check(first.status == DecodeStatus::Complete && first.consumed == wire.size() &&
              first.frame.requestId == max && decodeDataChunk(first.frame.payload).data == "abc", "frame composition");
        check(decode(std::string_view(wire + wire).substr(first.consumed)).status == DecodeStatus::Complete, "second frame");
        check(decode(encode(Frame{5, 1, largest})).frame.payload == largest, "max frame composition");
        std::cout << "DATA_CHUNK golden, bounds, range and frame composition passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
