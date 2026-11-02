#include "chunkharbor/protocol/FilesList.h"
#include "DeltaJson.h"
#include <algorithm>
namespace chunkharbor::protocol {
namespace {
using namespace delta_json;
void path(const std::string &s) {
    try { encodeBeginUpload({s, 0, 0, 1, std::string(64, '0')}); }
    catch (const BeginUploadError &) { bad(); }
}
void validate(const ListFiles &v) {
    if (!v.limit || v.limit > MaxFilesPerPage) bad();
    if (!v.afterPath.empty()) path(v.afterPath);
}
bool before(const std::string &a, const std::string &b) {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
        [](unsigned char x, unsigned char y) { return x < y; });
}
J entry(const ListedFile &f) {
    try { encodeBeginUpload({f.path, f.size, 0, 1, f.sha256}); }
    catch (const BeginUploadError &) { bad(); }
    id<16>(J(f.versionId));
    return J{{"path", f.path}, {"version_id", f.versionId}, {"size", f.size}, {"sha256", f.sha256}};
}
} // namespace
std::string encodeListFiles(const ListFiles &v) {
    validate(v);
    return dump(J{{"after_path", v.afterPath}, {"limit", v.limit}});
}
ListFiles decodeListFiles(std::string_view s) {
    auto j = parse(s);
    fields(j, {"after_path", "limit"});
    ListFiles v{str(j.at("after_path")), static_cast<std::uint32_t>(num(j.at("limit"), MaxFilesPerPage))};
    validate(v);
    return v;
}
std::string encodeFilesResult(const FilesResult &v) {
    if (v.files.size() > MaxFilesPerPage ||
        (!v.nextAfterPath.empty() && (v.files.empty() || v.nextAfterPath != v.files.back().path)))
        bad();
    J items = J::array();
    std::string previous;
    for (const auto &f : v.files) {
        if (!previous.empty() && !before(previous, f.path)) bad();
        items.push_back(entry(f));
        previous = f.path;
    }
    return dump(J{{"files", items}, {"next_after_path", v.nextAfterPath}});
}
FilesResult decodeFilesResult(std::string_view s) {
    auto j = parse(s);
    fields(j, {"files", "next_after_path"});
    const auto &items = j.at("files");
    if (!items.is_array() || items.size() > MaxFilesPerPage) bad();
    FilesResult v;
    v.nextAfterPath = str(j.at("next_after_path"));
    for (const auto &f : items) {
        fields(f, {"path", "version_id", "size", "sha256"});
        v.files.push_back({str(f.at("path")), str(f.at("version_id")), num(f.at("size")), str(f.at("sha256"))});
    }
    encodeFilesResult(v);
    return v;
}
} // namespace chunkharbor::protocol
