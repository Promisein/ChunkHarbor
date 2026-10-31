#pragma once
#include "chunkharbor/protocol/BeginUpload.h"
namespace chunkharbor::protocol {
class UploadFailure : public std::runtime_error {
  public:
    UploadFailure(std::string code, std::string message)
        : std::runtime_error(std::move(message)), code(std::move(code)) {}
    std::string code;
};
struct CommitUpload {
    std::array<std::uint8_t, 16> sessionId{};
};
struct CommitResult {
    std::array<std::uint8_t, 16> sessionId{};
    std::string path;
    std::uint64_t size = 0;
    std::string sha256;
};
struct UploadError {
    std::string code, message;
};
std::string encodeCommitUpload(const CommitUpload &);
CommitUpload decodeCommitUpload(std::string_view);
std::string encodeCommitResult(const CommitResult &);
CommitResult decodeCommitResult(std::string_view);
std::string encodeUploadError(const UploadError &);
UploadError decodeUploadError(std::string_view);
} // namespace chunkharbor::protocol
