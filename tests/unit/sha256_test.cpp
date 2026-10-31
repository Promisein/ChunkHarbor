#include "chunkharbor/common/Sha256.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>

using namespace chunkharbor;
void check(bool ok) { if (!ok) throw std::runtime_error("SHA-256 check failed"); }
template<class E, class F> void rejects(F fn) {
    try { fn(); } catch (const E&) { return; }
    throw std::runtime_error("expected exception");
}
struct TempDirectory {
    std::filesystem::path path;
    TempDirectory() {
        std::random_device random;
        for (int i = 0; i < 100; ++i) {
            auto candidate = std::filesystem::temp_directory_path() /
                ("chunkharbor-sha256-" + std::to_string(random()));
            if (std::filesystem::create_directory(candidate)) { path = candidate; return; }
        }
        throw std::runtime_error("cannot create test directory");
    }
    ~TempDirectory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};
struct BrokenInput : std::streambuf {
    int_type underflow() override { throw std::runtime_error("injected read failure"); }
};
struct BrokenAfterPrefix : std::stringbuf {
    BrokenAfterPrefix() : std::stringbuf(std::string(65536, 'a')) {}
    int_type underflow() override { throw std::runtime_error("read failure after prefix"); }
};
int main() {
    try {
        const std::string empty = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
        const std::string abc = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
        check(toHex(sha256({})) == empty);
        check(toHex(sha256("abc")) == abc);
        check(toHex(sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
        std::string million(1000000, 'a');
        const std::string millionHash = "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0";
        check(toHex(sha256(million)) == millionHash);
        Sha256 incremental;
        incremental.update({}); incremental.update("a"); incremental.update("bc");
        check(toHex(incremental.finish()) == abc);
        rejects<std::logic_error>([&] { incremental.update("x"); });
        rejects<std::logic_error>([&] { incremental.finish(); });
        // Explicit length must preserve NUL and bytes with the high bit set.
        std::string binary;
        for (int i = 0; i < 256; ++i) binary.push_back(static_cast<char>(i));
        check(toHex(sha256(binary)) == "40aff2e9d2d8922e47afd4648e6967497158785fbd1da870e7110266bf944880");
        for (const auto& bytes : {std::string{}, std::string("abc"), binary, million}) {
            std::istringstream input(bytes);
            check(sha256Stream(input) == sha256(bytes));
            std::istringstream throwingInput(bytes);
            throwingInput.exceptions(std::ios::failbit | std::ios::badbit);
            check(sha256Stream(throwingInput) == sha256(bytes));
        }
        BrokenInput broken;
        std::istream bad(&broken);
        rejects<std::runtime_error>([&] { sha256Stream(bad); });
        BrokenAfterPrefix prefix;
        std::istream badAfterPrefix(&prefix);
        rejects<std::runtime_error>([&] { sha256Stream(badAfterPrefix); });
        std::istringstream alreadyFailed("abc");
        alreadyFailed.setstate(std::ios::failbit);
        rejects<std::runtime_error>([&] { sha256Stream(alreadyFailed); });
        TempDirectory temp;
        const auto file = temp.path / std::filesystem::u8path(u8"中文-fixture.bin");
        for (const auto& bytes : {std::string{}, std::string("abc"), binary, million}) {
            { std::ofstream out(file, std::ios::binary); out.write(bytes.data(), bytes.size()); check(bool(out)); }
            check(sha256File(file) == sha256(bytes));
        }
        check(toHex(sha256File(file)) == millionHash);
        // Independent .NET SHA256 reference values around the 64 KiB read boundary.
        const char* boundaryHashes[] = {
            "6e1bebca6a8229364a162a72ef064826c4cd7457bf54f190ef782bd9deff3e42",
            "bf718b6f653bebc184e1479f1935b8da974d701b893afcf49e701f3e2f9f9c5a",
            "008ffc88d3c96a9f307524eb361e47c5222a887fc45fa0c1fb8d429c5c23b430"};
        for (std::size_t i = 0; i < 3; ++i) {
            const std::string bytes(65535 + i, 'a');
            std::istringstream input(bytes);
            check(toHex(sha256Stream(input)) == boundaryHashes[i]);
            std::istringstream throwing(bytes);
            throwing.exceptions(std::ios::failbit | std::ios::badbit | std::ios::eofbit);
            check(toHex(sha256Stream(throwing)) == boundaryHashes[i]);
            { std::ofstream out(file, std::ios::binary); out.write(bytes.data(), bytes.size()); check(bool(out)); }
            check(toHex(sha256File(file)) == boundaryHashes[i]);
        }
        std::istringstream suffix("ignoreabc");
        suffix.seekg(6);
        check(toHex(sha256Stream(suffix)) == abc);
        rejects<std::runtime_error>([&] { sha256File(temp.path / "missing"); });
        // Independent NIST vector also exercises many update boundaries.
        Sha256 repeated;
        for (int i = 0; i < 1000; ++i) repeated.update(std::string_view(million.data(), 1000));
        check(toHex(repeated.finish()) == millionHash);
        std::cout << "SHA-256 known vectors, streaming, binary and I/O errors passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
