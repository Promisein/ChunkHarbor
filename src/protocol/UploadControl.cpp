#include "chunkharbor/protocol/UploadControl.h"
#include <nlohmann/json.hpp>
#include <set>
namespace chunkharbor::protocol {
namespace {
using J = nlohmann::json;
[[noreturn]] void bad() {
    throw UploadFailure("MALFORMED_MESSAGE", "Invalid upload control message");
}
J parse(std::string_view s, std::initializer_list<const char *> fields) {
    if (s.empty() || s.size() > MaxControlPayload || s.find('\0') != s.npos ||
        static_cast<unsigned char>(s.front()) == 0xef)
        bad();
    std::set<std::string> allowed(fields.begin(), fields.end()), seen;
    auto cb = [&](int depth, J::parse_event_t event, J &v) {
        if (depth > 1 || event == J::parse_event_t::array_start ||
            (event == J::parse_event_t::object_start && depth != 0))
            bad();
        if (event == J::parse_event_t::key) {
            auto k = v.get<std::string>();
            if (!allowed.count(k) || !seen.insert(k).second)
                bad();
        }
        return true;
    };
    auto j = J::parse(s.begin(), s.end(), cb, true, false);
    if (!j.is_object() || seen != allowed)
        bad();
    return j;
}
std::string str(const J &v) {
    if (!v.is_string())
        bad();
    return v.get<std::string>();
}
std::string hex(const std::array<std::uint8_t, 16> &id) {
    std::string s;
    for (auto b : id) {
        s += "0123456789abcdef"[b >> 4];
        s += "0123456789abcdef"[b & 15];
    }
    return s;
}
std::array<std::uint8_t, 16> id(const J &v) {
    auto s = str(v);
    if (s.size() != 32)
        bad();
    std::array<std::uint8_t, 16> r{};
    for (size_t i = 0; i < 32; ++i) {
        auto p = std::string_view("0123456789abcdef").find(s[i]);
        if (p == std::string_view::npos)
            bad();
        r[i / 2] = static_cast<std::uint8_t>((r[i / 2] << 4) | p);
    }
    return r;
}
void validate(const CommitResult &r) {
    try {
        encodeBeginUpload({r.path, r.size, 0, 262144, r.sha256});
    } catch (const std::exception &) {
        bad();
    }
}
void validate(const UploadError &e) {
    static const std::set<std::string> codes = {"MALFORMED_MESSAGE",
                                                "HANDSHAKE_REQUIRED",
                                                "ALREADY_HANDSHAKEN",
                                                "UNSUPPORTED_VERSION",
                                                "UNSUPPORTED_OPERATION",
                                                "CONFLICT",
                                                "QUOTA_EXCEEDED",
                                                "DISK_FULL",
                                                "CHECKSUM_MISMATCH",
                                                "SESSION_NOT_FOUND",
                                                "BUSY",
                                                "INTERNAL_ERROR",
                                                "INVALID_PATH",
                                                "INCOMPLETE_UPLOAD",
                                                "SOURCE_CHANGED"};
    if (!codes.count(e.code) || e.message.empty() || e.message.size() > 160)
        bad();
    for (unsigned char c : e.message)
        if (c < 32 || c > 126)
            bad();
}
} // namespace
std::string encodeCommitUpload(const CommitUpload &r) {
    return J{{"session_id", hex(r.sessionId)}}.dump();
}
CommitUpload decodeCommitUpload(std::string_view s) {
    try {
        auto j = parse(s, {"session_id"});
        return {id(j.at("session_id"))};
    } catch (const J::exception &) {
        bad();
    }
}
std::string encodeCommitResult(const CommitResult &r) {
    validate(r);
    return J{{"session_id", hex(r.sessionId)},
             {"path", r.path},
             {"size", r.size},
             {"sha256", r.sha256},
             {"committed", true}}
        .dump();
}
CommitResult decodeCommitResult(std::string_view s) {
    try {
        auto j = parse(s, {"session_id", "path", "size", "sha256", "committed"});
        auto &n = j.at("size");
        if (!n.is_number_integer() || (!n.is_number_unsigned() && n.get<std::int64_t>() < 0) ||
            !j.at("committed").is_boolean() || !j.at("committed").get<bool>())
            bad();
        CommitResult r{id(j.at("session_id")), str(j.at("path")), n.get<std::uint64_t>(),
                       str(j.at("sha256"))};
        validate(r);
        return r;
    } catch (const J::exception &) {
        bad();
    }
}
std::string encodeUploadError(const UploadError &e) {
    validate(e);
    return J{{"code", e.code}, {"message", e.message}}.dump();
}
UploadError decodeUploadError(std::string_view s) {
    try {
        auto j = parse(s, {"code", "message"});
        UploadError e{str(j.at("code")), str(j.at("message"))};
        validate(e);
        return e;
    } catch (const J::exception &) {
        bad();
    }
}
} // namespace chunkharbor::protocol
