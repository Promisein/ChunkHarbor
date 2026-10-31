#pragma once
#include "chunkharbor/protocol/DataChunk.h"

namespace chunkharbor::protocol {
constexpr std::size_t MaxLogicalPathBytes = 4096;
constexpr std::size_t MaxPathComponentBytes = 255;
class BeginUploadError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
struct BeginUpload {
    std::string path;
    std::uint64_t size = 0;
    std::int64_t mtimeNs = 0;
    std::uint32_t chunkSize = 256 * 1024;
    std::string sha256;
};
// JSON body only; Frame type 3, request association and upload state are external.
// Logical path validation is lexical, not filesystem confinement.
std::string encodeBeginUpload(const BeginUpload& value);
BeginUpload decodeBeginUpload(std::string_view payload);
} // namespace chunkharbor::protocol
