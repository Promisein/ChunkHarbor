#include "chunkharbor/protocol/DataChunk.h"
#include "chunkharbor/protocol/ManifestPage.h"
#include <iostream>
#include <limits>
using namespace chunkharbor::protocol;
void check(bool v) {
    if (!v)
        throw std::runtime_error("manifest assertion failed");
}
template <class F> void rejects(F f) {
    try {
        f();
    } catch (const ManifestPageError &) {
        return;
    }
    throw std::runtime_error("expected manifest rejection");
}
int main() {
    try {
        ManifestPage p;
        p.hashes.resize(1);
        const std::string golden = "{\"first_block_index\":0,\"hashes\":[\"" +
                                   std::string(64, '0') + "\"],\"page_index\":0,\"session_id\":\"" +
                                   std::string(32, '0') + "\"}";
        check(encodeManifestPage(p) == golden);
        check(decodeManifestPage(golden).hashes == p.hashes);
        auto wire = encode({18, 1, golden});
        const auto expectedHeader = std::string("CHKH\0\1\0\x12", 8) + std::string(11, '\0') +
                                    std::string(1, '\1') + std::string(7, '\0') +
                                    std::string(1, static_cast<char>(golden.size())) +
                                    std::string(4, '\0');
        check(golden.size() < 256 && wire == expectedHeader + golden);
        for (std::size_t n = 0; n < wire.size(); ++n)
            check(decode(std::string_view(wire).substr(0, n)).status == DecodeStatus::NeedMore);
        auto joined = wire + encode({16, 2, ""});
        auto frame = decode(joined);
        check(frame.status == DecodeStatus::Complete && frame.consumed == wire.size());
        check(decodeManifestPage(frame.frame.payload).hashes == p.hashes);
        check(decode(std::string_view(joined).substr(frame.consumed)).frame.type == 16);
        auto rejectText = [&](std::string s) { rejects([&] { decodeManifestPage(s); }); };
        for (auto s : {"", "{}", "[]", "null", "{", "{\"hashes\":[[]]}", "{\"hashes\":[{}]}",
                       "{\"hashes\":[1]}"})
            rejectText(s);
        rejectText(golden + std::string(1, '\0') + "garbage");
        rejectText(std::string("\xef\xbb\xbf") + golden);
        rejectText(golden + "x");
        for (auto member : {"\"page_index\":0", "\"unknown\":0"})
            rejectText(golden.substr(0, golden.size() - 1) + "," + member + "}");
        for (auto number : {"-1", "0.0", "4294967296", "18446744073709551616", "128"}) {
            auto s = golden;
            auto at = s.find("\"page_index\":0");
            s.replace(at, 14, std::string("\"page_index\":") + number);
            rejectText(s);
        }
        auto invalid = golden;
        invalid[invalid.find(std::string(64, '0'))] = 'A';
        rejectText(invalid);
        invalid = golden;
        invalid[invalid.find(std::string(64, '0'))] = static_cast<char>(0xff);
        rejectText(invalid);
        for (const auto &id : {std::string(), std::string(31, '0'), std::string(33, '0'),
                               std::string(32, 'g'), std::string(32, 'A')}) {
            auto s = golden;
            s.replace(s.rfind(std::string(32, '0')), 32, id);
            rejectText(s);
        }
        for (const auto &digest :
             {std::string(), std::string(63, '0'), std::string(65, '0'), std::string(64, 'g')}) {
            auto s = golden;
            s.replace(s.find(std::string(64, '0')), 64, digest);
            rejectText(s);
        }
        invalid = golden;
        invalid.replace(invalid.find('['), invalid.find(']') - invalid.find('[') + 1, "[]");
        rejectText(invalid);
        invalid = golden;
        invalid.replace(invalid.rfind(std::string(32, '0')) - 1, 34, "0");
        rejectText(invalid);
        check(decodeManifestPage(golden + std::string(MaxControlPayload - golden.size(), ' '))
                  .hashes.size() == 1);
        rejectText(golden + std::string(MaxControlPayload - golden.size() + 1, ' '));
        validateManifestPageForFile(p, p.sessionId, 1, 1, 0);
        rejects([&] { validateManifestPageForFile(p, p.sessionId, 0, 1, 0); });
        rejects([&] { validateManifestPageForFile(p, p.sessionId, 513, 1, 0); });
        rejects([&] { validateManifestPageForFile(p, p.sessionId, 1, 0, 0); });
        rejects([&] { validateManifestPageForFile(p, p.sessionId, 1, MaxChunkDataSize + 1, 0); });
        rejects([&] { validateManifestPageForFile(p, p.sessionId, 1, 1, 1); });
        auto other = p.sessionId;
        other[0] = 1;
        rejects([&] { validateManifestPageForFile(p, other, 1, 1, 0); });
        rejects([&] {
            validateManifestPageForFile(p, p.sessionId, std::numeric_limits<std::uint64_t>::max(),
                                        1, 0);
        });
        rejects([&] { validateManifestPageForFile(p, p.sessionId, 65537, 1, 0); });
        for (std::uint32_t count : {1U, 511U, 512U}) {
            p.hashes.resize(count);
            validateManifestPageForFile(p, p.sessionId, count * 2 - 1, 2, 0);
            check(decodeManifestPage(encodeManifestPage(p)).hashes == p.hashes);
        }
        p.hashes.resize(513);
        rejects([&] { encodeManifestPage(p); });
        // Reject oversized decoded arrays, not merely oversized encoded objects.
        auto oversized = golden;
        const auto end = oversized.find(']');
        for (int i = 0; i < 512; ++i)
            oversized.insert(end, ",\"" + std::string(64, '0') + "\"");
        rejectText(oversized);
        p.hashes.clear();
        rejects([&] { encodeManifestPage(p); });
        p.hashes.resize(1);
        p.pageIndex = 1;
        p.firstBlockIndex = 512;
        validateManifestPageForFile(p, p.sessionId, 513, 1, 1);
        rejects([&] { validateManifestPageForFile(p, p.sessionId, 512, 1, 1); });
        p.pageIndex = 127;
        p.firstBlockIndex = 65024;
        p.hashes.resize(512);
        validateManifestPageForFile(p, p.sessionId, 65536, 1, 127);
        check(decodeManifestPage(encodeManifestPage(p)).hashes.size() == 512);
        p.firstBlockIndex = 65025;
        rejects([&] { encodeManifestPage(p); });
        p.pageIndex = 128;
        p.firstBlockIndex = 65536;
        rejects([&] { encodeManifestPage(p); });
        p.pageIndex = 0;
        p.firstBlockIndex = 0;
        p.hashes.resize(1);
        for (std::size_t i = 0; i < p.sessionId.size(); ++i)
            p.sessionId[i] = static_cast<std::uint8_t>(i);
        for (std::size_t i = 0; i < 32; ++i)
            p.hashes[0][i] = static_cast<std::uint8_t>(255 - i);
        auto owned = decodeManifestPage(encodeManifestPage(p));
        check(owned.sessionId == p.sessionId && owned.hashes == p.hashes);
        rejects([&] {
            validateManifestPageForFile(p, p.sessionId, 64ULL * 1024 * 1024 + 1, MaxChunkDataSize,
                                        0);
        });
        std::cout << "manifest page passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
