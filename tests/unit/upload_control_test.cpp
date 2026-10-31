#include "chunkharbor/protocol/UploadControl.h"
#include <iostream>
using namespace chunkharbor::protocol;
void check(bool b) {
    if (!b)
        throw std::runtime_error("check failed");
}
template <class F> void rejects(F f) {
    try {
        f();
    } catch (const UploadFailure &) {
        return;
    }
    throw std::runtime_error("expected rejection");
}
int main() {
    try {
        CommitResult r{};
        r.path = "dir/file";
        r.size = 9007199254740993ULL;
        r.sha256 = std::string(64, 'a');
        auto valid = encodeCommitResult(r);
        check(decodeCommitResult(valid).size == r.size);
        check(decodeCommitUpload(encodeCommitUpload({})).sessionId == r.sessionId);
        for (auto text : {"{}", "[]", "{\"session_id\":1}",
                          "{\"session_id\":\"00000000000000000000000000000000\",\"session_id\":"
                          "\"00000000000000000000000000000000\"}"})
            rejects([&] { decodeCommitUpload(text); });
        for (auto suffix : {",\"extra\":1}", ",\"committed\":true}"})
            rejects([&] { decodeCommitResult(valid.substr(0, valid.size() - 1) + suffix); });
        for (auto size : {"-1", "1.0", "1e3", "18446744073709551616", "null", "[]"}) {
            auto v = valid;
            auto p = v.find("9007199254740993");
            v.replace(p, 16, size);
            rejects([&] { decodeCommitResult(v); });
        }
        auto v = valid;
        auto p = v.find("true");
        v.replace(p, 4, "false");
        rejects([&] { decodeCommitResult(v); });
        rejects([&] { decodeCommitResult(std::string("\xef\xbb\xbf") + valid); });
        rejects([&] { decodeCommitResult(valid + std::string(1, '\0')); });
        check(decodeCommitResult(valid + std::string(MaxControlPayload - valid.size(), ' ')).path ==
              r.path);
        rejects([&] { decodeCommitResult(valid + std::string(MaxControlPayload, ' ')); });
        r.path = "../escape";
        rejects([&] { encodeCommitResult(r); });
        r.path = "ok";
        r.sha256 = "ABC";
        rejects([&] { encodeCommitResult(r); });
        check(decodeUploadError(encodeUploadError({"DISK_FULL", "Storage unavailable"})).code ==
              "DISK_FULL");
        rejects([] { encodeUploadError({"ARBITRARY", "bad"}); });
        rejects([] { encodeUploadError({"BUSY", "bad\nmessage"}); });
        for (const auto *code :
             {"MALFORMED_MESSAGE", "HANDSHAKE_REQUIRED", "ALREADY_HANDSHAKEN",
              "UNSUPPORTED_VERSION", "UNSUPPORTED_OPERATION", "CONFLICT", "QUOTA_EXCEEDED",
              "DISK_FULL", "CHECKSUM_MISMATCH", "SESSION_NOT_FOUND", "BUSY", "INTERNAL_ERROR",
              "INVALID_PATH", "INCOMPLETE_UPLOAD", "SOURCE_CHANGED"})
            check(decodeUploadError(encodeUploadError({code, "Safe message"})).code == code);
        const auto error = encodeUploadError({"BUSY", std::string(160, 'x')});
        check(decodeUploadError(error).message.size() == 160);
        rejects([] { encodeUploadError({"BUSY", std::string(161, 'x')}); });
        rejects([] { encodeUploadError({"BUSY", std::string(1, static_cast<char>(127))}); });
        for (const auto *text : {"[]", "{}", "{\"code\":\"BUSY\",\"message\":{}}",
                                 "{\"code\":\"BUSY\",\"message\":\"ok\",\"extra\":0}",
                                 "{\"code\":\"BUSY\",\"code\":\"BUSY\",\"message\":\"ok\"}",
                                 "{\"code\":\"BUSY\",\"message\":\"a\\u0000b\"}"})
            rejects([&] { decodeUploadError(text); });
        for (const auto *text :
             {"{\"session_id\":{}}", "{\"session_id\":[]}",
              "{\"session_id\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\"}",
              "{\"session_id\":\"00000000000000000000000000000000\",\"extra\":0}"})
            rejects([&] { decodeCommitUpload(text); });
        r.path = "ok";
        r.sha256 = std::string(64, 'b');
        r.size = UINT64_MAX;
        check(decodeCommitResult(encodeCommitResult(r)).size == UINT64_MAX);
        std::cout << "Upload control tests passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
