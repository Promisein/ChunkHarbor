#pragma once
#include "chunkharbor/protocol/UploadControl.h"
namespace chunkharbor::protocol {
constexpr std::uint32_t MaxDownloadChunkSize = 256 * 1024;
constexpr std::uint64_t MaxDownloadFileSize = 64ULL * 1024 * 1024;
struct BeginDownload { std::string path, versionId; }; // empty version: current
struct DownloadReady {
    std::string path, versionId;
    std::uint64_t size = 0;
    std::string sha256;
};
struct GetDownloadChunk {
    std::string versionId;
    std::uint64_t offset = 0;
    std::uint32_t length = 0;
};
struct DownloadChunk {
    std::string versionId;
    std::uint64_t offset = 0;
    std::array<std::uint8_t, 32> sha256{};
    std::string data;
};
// Control types 13/32/33. Type14 reuses DATA_CHUNK's explicit big-endian body,
// with version ID replacing session ID; these functions perform no I/O/hashing.
std::string encodeBeginDownload(const BeginDownload &);
BeginDownload decodeBeginDownload(std::string_view);
std::string encodeDownloadReady(const DownloadReady &);
DownloadReady decodeDownloadReady(std::string_view);
std::string encodeGetDownloadChunk(const GetDownloadChunk &);
GetDownloadChunk decodeGetDownloadChunk(std::string_view);
std::string encodeDownloadChunk(const DownloadChunk &);
DownloadChunk decodeDownloadChunk(std::string_view);
} // namespace chunkharbor::protocol
