#include "chunkharbor/net/Buffer.h"

#include <cstring>
#include <algorithm>
#include <deque>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

using chunkharbor::Buffer;
void check(bool value) { if (!value) throw std::runtime_error("Buffer check failed"); }
template<class Error, class Function> void rejects(Function fn) {
    try { fn(); } catch (const Error&) { return; }
    throw std::runtime_error("expected exception missing");
}

int main() {
    try {
        Buffer b(4, 16);
        check(b.readableBytes() == 0 && b.capacity() == 4 && b.writableBytes() == 4);
        check(b.read(0).empty() && b.readableView().empty());
        rejects<std::out_of_range>([&] { b.consume(1); });
        rejects<std::out_of_range>([&] { b.read(1); });
        b.append("ab"); b.append("cd");
        check(b.read(2) == "ab");
        b.append("ef"); // compaction without growth
        check(b.capacity() == 4 && b.readableView() == "cdef");
        b.append("ghijk");
        check(b.capacity() >= 9 && b.capacity() <= 16 && b.readableView() == "cdefghijk");
        b.append(b.readableView().substr(0, 3)); // alias survives growth/compaction
        check(b.readAll() == "cdefghijkcde");
        b.ensureWritable(16);
        std::memset(b.writableData(), 'x', 16); b.commitWrite(16);
        rejects<std::length_error>([&] { b.append("!"); });
        rejects<std::out_of_range>([&] { b.commitWrite(1); });
        rejects<std::length_error>([&] { b.ensureWritable(std::numeric_limits<std::size_t>::max()); });
        check(b.readableBytes() == 16 && b.readAll() == std::string(16, 'x'));
        b.append(std::string_view("a\0b", 3));
        check(b.readAll() == std::string("a\0b", 3));
        Buffer zero(0, 0);
        zero.append({}); zero.ensureWritable(0); zero.commitWrite(0); zero.consume(0);
        check(zero.readAll().empty());
        rejects<std::length_error>([&] { zero.append("x"); });
        rejects<std::invalid_argument>([] { Buffer bad(2, 1); });

        Buffer stream(0, 64);
        std::deque<char> expected;
        unsigned state = 12345;
        for (int i = 0; i < 5000; ++i) {
            state = state * 1664525u + 1013904223u;
            const auto n = static_cast<std::size_t>(state % 9);
            if ((state & 16u) && n <= 64 - expected.size()) {
                std::string part(n, static_cast<char>(state % 256));
                stream.append(part);
                expected.insert(expected.end(), part.begin(), part.end());
            } else {
                const auto count = std::min(n, expected.size());
                stream.consume(count);
                expected.erase(expected.begin(), expected.begin() + count);
            }
            check(stream.readableView() == std::string(expected.begin(), expected.end()));
            check(stream.capacity() <= 64 && stream.writableBytes() <= stream.capacity());
        }
        std::cout << "Buffer boundary and 5000 stream operations passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
