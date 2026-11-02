#include "chunkharbor/protocol/Download.h"
#include "DeltaJson.h"
namespace chunkharbor::protocol {
namespace {
using namespace delta_json;
void validate(const BeginDownload &v) {
    try { encodeBeginUpload({v.path, 0, 0, 1, std::string(64, '0')}); }
    catch (const BeginUploadError &) { bad(); }
    if (!v.versionId.empty()) id<16>(J(v.versionId));
}
void validate(const DownloadReady &v) {
    validate(BeginDownload{v.path, v.versionId});
    id<16>(J(v.versionId));
    if (v.size > MaxDownloadFileSize) bad();
    try { encodeBeginUpload({v.path, v.size, 0, 1, v.sha256}); }
    catch (const BeginUploadError &) { bad(); }
}
void validate(const GetDownloadChunk &v) {
    id<16>(J(v.versionId));
    if (!v.length || v.length > MaxDownloadChunkSize || v.offset > MaxDownloadFileSize ||
        v.length > MaxDownloadFileSize - v.offset) bad();
}
} // namespace
std::string encodeBeginDownload(const BeginDownload &v) {
    validate(v);
    return dump(J{{"path", v.path}, {"version_id", v.versionId.empty() ? J(nullptr) : J(v.versionId)}});
}
BeginDownload decodeBeginDownload(std::string_view s) {
    auto j = parse(s);
    fields(j, {"path", "version_id"});
    BeginDownload v{str(j.at("path")), j.at("version_id").is_null() ? "" : str(j.at("version_id"))};
    // Empty string is not the wire representation of an absent precondition.
    if (!j.at("version_id").is_null() && v.versionId.empty()) bad();
    validate(v);
    return v;
}
std::string encodeDownloadReady(const DownloadReady &v) {
    validate(v);
    return dump(J{{"path", v.path}, {"version_id", v.versionId}, {"size", v.size}, {"sha256", v.sha256}});
}
DownloadReady decodeDownloadReady(std::string_view s) {
    auto j = parse(s);
    fields(j, {"path", "version_id", "size", "sha256"});
    DownloadReady v{str(j.at("path")), str(j.at("version_id")), num(j.at("size")), str(j.at("sha256"))};
    validate(v);
    return v;
}
std::string encodeGetDownloadChunk(const GetDownloadChunk &v) {
    validate(v);
    return dump(J{{"version_id", v.versionId}, {"offset", v.offset}, {"length", v.length}});
}
GetDownloadChunk decodeGetDownloadChunk(std::string_view s) {
    auto j = parse(s);
    fields(j, {"version_id", "offset", "length"});
    GetDownloadChunk v{str(j.at("version_id")), num(j.at("offset")),
                       static_cast<std::uint32_t>(num(j.at("length"), MaxDownloadChunkSize))};
    validate(v);
    return v;
}
std::string encodeDownloadChunk(const DownloadChunk &v) {
    if (v.data.size() > MaxDownloadChunkSize) bad();
    return encodeDataChunk({id<16>(J(v.versionId)), v.offset, v.sha256, v.data});
}
DownloadChunk decodeDownloadChunk(std::string_view s) {
    if (s.size() > DataChunkHeaderSize + MaxDownloadChunkSize) bad();
    auto v = decodeDataChunk(s);
    return {hex(v.sessionId), v.offset, v.sha256, std::move(v.data)};
}
} // namespace chunkharbor::protocol
