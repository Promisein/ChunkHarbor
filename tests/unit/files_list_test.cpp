#ifdef NDEBUG
#undef NDEBUG
#endif
#include "chunkharbor/protocol/FilesList.h"
#include <cassert>
#include <functional>
using namespace chunkharbor::protocol;
static void rejects(const std::function<void()> &f) {
    bool caught = false;
    try { f(); } catch (const UploadFailure &e) { caught = e.code == "MALFORMED_MESSAGE"; }
    assert(caught);
}
int main() {
    auto q = decodeListFiles(R"({"after_path":"","limit":1})");
    assert(q.afterPath.empty() && q.limit == 1);
    assert(decodeListFiles(encodeListFiles({"a/b", 64})).afterPath == "a/b");
    for (auto s : {R"({"after_path":"","limit":0})", R"({"after_path":"","limit":65})",
                   R"({"after_path":"","limit":1.0})", R"({"after_path":"","limit":-1})",
                   R"({"after_path":"../a","limit":1})", R"({"after_path":"/a","limit":1})",
                   R"({"after_path":"C:\\a","limit":1})", R"({"after_path":"","limit":1,"limit":2})",
                   R"({"after_path":"","limit":1,"extra":0})", R"({"limit":1})",
                   R"({"after_path":"a\u0000b","limit":1})"})
        rejects([&] { decodeListFiles(s); });
    FilesResult r{{{"a", std::string(32, 'a'), 9007199254740993ULL, std::string(64, 'b')}}, "a"};
    auto decoded = decodeFilesResult(encodeFilesResult(r));
    assert(decoded.files[0].size == 9007199254740993ULL && decoded.nextAfterPath == "a");
    assert(decodeFilesResult(R"({"files":[],"next_after_path":""})").files.empty());
    rejects([] { decodeFilesResult(R"({"files":[],"next_after_path":"a"})"); });
    r.nextAfterPath = "b";
    rejects([&] { encodeFilesResult(r); });
    r.nextAfterPath.clear();
    r.files.push_back(r.files[0]);
    rejects([&] { encodeFilesResult(r); });
    r.files.pop_back();
    r.files[0].path = "../bad";
    rejects([&] { encodeFilesResult(r); });
    r.files[0].path = "a";
    r.files[0].versionId = std::string(32, 'Z');
    rejects([&] { encodeFilesResult(r); });
    rejects([] { decodeListFiles(std::string(65537, ' ')); });
    rejects([] { decodeListFiles("{\"after_path\":\"\xff\",\"limit\":1}"); });
    const auto file = std::string(R"({"path":"a","version_id":")") + std::string(32, 'a') +
                      R"(","size":1.5,"sha256":")" + std::string(64, 'b') + R"("})";
    rejects([&] { decodeFilesResult("{\"files\":[" + file + "],\"next_after_path\":\"\"}"); });
    FilesResult many;
    for (int i = 0; i < 65; ++i)
        many.files.push_back({"a" + std::to_string(100 + i), std::string(32, 'a'), 0, std::string(64, 'b')});
    rejects([&] { encodeFilesResult(many); });
}
