#pragma once
#include "chunkharbor/protocol/UploadControl.h"
#include <nlohmann/json.hpp>
#include <set>
namespace chunkharbor::protocol::delta_json {
using J = nlohmann::json;
[[noreturn]] inline void bad() {
    throw UploadFailure("MALFORMED_MESSAGE", "Invalid delta control message");
}
inline J parse(std::string_view s) {
    if (s.empty() || s.size() > MaxControlPayload || s.find('\0') != s.npos ||
        static_cast<unsigned char>(s.front()) == 0xef)
        bad();
    std::vector<std::set<std::string>> keys(6);
    std::vector<std::size_t> counts(6);
    try {
        auto cb = [&](int depth, J::parse_event_t ev, J &v) {
            if (depth > 4)
                bad();
            if (ev == J::parse_event_t::object_start)
                keys.at(depth).clear();
            if (ev == J::parse_event_t::key &&
                !keys.at(depth - 1).insert(v.get<std::string>()).second)
                bad();
            if (ev == J::parse_event_t::array_start)
                counts.at(depth) = 0;
            if ((ev == J::parse_event_t::value || ev == J::parse_event_t::array_start ||
                 ev == J::parse_event_t::object_start) &&
                depth > 0 && ++counts.at(depth - 1) > 2048)
                bad();
            if (ev == J::parse_event_t::value && v.is_string() &&
                v.get_ref<const std::string &>().find('\0') != std::string::npos)
                bad();
            return true;
        };
        return J::parse(s.begin(), s.end(), cb, true, false);
    } catch (const J::exception &) {
        bad();
    }
}
inline void fields(const J &j, std::initializer_list<const char *> names) {
    if (!j.is_object() || j.size() != names.size())
        bad();
    for (auto n : names)
        if (!j.contains(n))
            bad();
}
inline std::string str(const J &j) {
    if (!j.is_string())
        bad();
    return j.get<std::string>();
}
inline std::uint64_t num(const J &j, std::uint64_t max = UINT64_MAX) {
    if (!j.is_number_integer() || (!j.is_number_unsigned() && j.get<std::int64_t>() < 0))
        bad();
    auto n = j.get<std::uint64_t>();
    if (n > max)
        bad();
    return n;
}
template <std::size_t N> std::string hex(const std::array<std::uint8_t, N> &a) {
    std::string s;
    for (auto b : a) {
        s += "0123456789abcdef"[b >> 4];
        s += "0123456789abcdef"[b & 15];
    }
    return s;
}
template <std::size_t N> std::array<std::uint8_t, N> id(const J &j) {
    auto s = str(j);
    if (s.size() != N * 2)
        bad();
    std::array<std::uint8_t, N> a{};
    for (std::size_t i = 0; i < s.size(); ++i) {
        auto n = std::string_view("0123456789abcdef").find(s[i]);
        if (n == std::string_view::npos)
            bad();
        a[i / 2] = static_cast<std::uint8_t>((a[i / 2] << 4) | n);
    }
    return a;
}
inline J base(const std::optional<VersionId> &v) { return v ? J(hex(*v)) : J(nullptr); }
inline std::optional<VersionId> base(const J &j) {
    if (j.is_null())
        return {};
    return id<16>(j);
}
inline std::string dump(const J &j) {
    auto s = j.dump();
    if (s.size() > MaxControlPayload)
        bad();
    return s;
}
} // namespace chunkharbor::protocol::delta_json
