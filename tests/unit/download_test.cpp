#ifdef NDEBUG
#undef NDEBUG
#endif
#include "chunkharbor/protocol/Download.h"
#include <cassert>
#include <functional>
using namespace chunkharbor::protocol;
static void rejects(const std::function<void()> &f) {
    bool bad = false;
    try { f(); } catch (const std::exception &) { bad = true; }
    assert(bad);
}
int main() {
    auto b = decodeBeginDownload(R"({"path":"docs/a","version_id":null})");
    assert(b.path == "docs/a" && b.versionId.empty());
    assert(decodeBeginDownload(encodeBeginDownload({"a", std::string(32, 'a')})).versionId == std::string(32, 'a'));
    for (auto input : {R"({"path":"../a","version_id":null})", R"({"path":"/a","version_id":null})",
                       R"({"path":"a","version_id":"BAD"})", R"({"path":"a","path":"b","version_id":null})",
                       R"({"path":"a","version_id":null,"extra":1})"})
        rejects([&] { decodeBeginDownload(input); });
    DownloadReady ready{"a", std::string(32, 'a'), 67108864, std::string(64, 'b')};
    assert(decodeDownloadReady(encodeDownloadReady(ready)).size == 67108864);
    ready.size++;
    rejects([&] { encodeDownloadReady(ready); });
    GetDownloadChunk get{std::string(32, 'a'), 0, 262144};
    assert(decodeGetDownloadChunk(encodeGetDownloadChunk(get)).length == 262144);
    get.length = 0;
    rejects([&] { encodeGetDownloadChunk(get); });
    get.length = 262145;
    rejects([&] { encodeGetDownloadChunk(get); });
    rejects([] { decodeGetDownloadChunk(R"({"version_id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","offset":1.0,"length":1})"); });
    DownloadChunk chunk{std::string(32, '1'), 7, {}, std::string("a\0b", 3)};
    auto wire = encodeDownloadChunk(chunk);
    assert(wire.size() == 63 && static_cast<unsigned char>(wire[0]) == 0x11 && wire[23] == 7 && wire[27] == 3);
    auto decoded = decodeDownloadChunk(wire);
    assert(decoded.versionId == chunk.versionId && decoded.data == chunk.data && decoded.offset == 7);
    rejects([&] { decodeDownloadChunk(wire.substr(0, 62)); });
    chunk.data = std::string(262145, 'x');
    rejects([&] { encodeDownloadChunk(chunk); });
    assert(decode(encode({32, 1, "{}"})).frame.type == 32);
    assert(decode(encode({33, 2, "{}"})).frame.type == 33);
}
