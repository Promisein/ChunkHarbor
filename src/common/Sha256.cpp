#include "chunkharbor/common/Sha256.h"
#include <openssl/evp.h>
#include <fstream>
#include <istream>
#include <stdexcept>

namespace chunkharbor {
struct Sha256::Impl {
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context{
        EVP_MD_CTX_new(), &EVP_MD_CTX_free};
    bool active = true;
};
Sha256::Sha256() : impl_(std::make_unique<Impl>()) {
    if (!impl_->context || EVP_DigestInit_ex(impl_->context.get(), EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("SHA-256 initialization failed");
}
Sha256::~Sha256() = default;
void Sha256::update(std::string_view bytes) {
    if (!impl_->active) throw std::logic_error("SHA-256 context is terminal");
    if (!bytes.empty() && EVP_DigestUpdate(impl_->context.get(), bytes.data(), bytes.size()) != 1) {
        impl_->active = false;
        throw std::runtime_error("SHA-256 update failed");
    }
}
Sha256Digest Sha256::finish() {
    if (!impl_->active) throw std::logic_error("SHA-256 context is terminal");
    impl_->active = false;
    // EVP's generic interface accepts up to EVP_MAX_MD_SIZE output bytes.
    std::array<unsigned char, EVP_MAX_MD_SIZE> output{};
    unsigned int length = 0;
    if (EVP_DigestFinal_ex(impl_->context.get(), output.data(), &length) != 1 || length != 32)
        throw std::runtime_error("SHA-256 finalization failed");
    Sha256Digest digest{};
    for (std::size_t i = 0; i < digest.size(); ++i) digest[i] = output[i];
    return digest;
}
Sha256Digest sha256(std::string_view bytes) {
    Sha256 hash;
    hash.update(bytes);
    return hash.finish();
}
std::string toHex(const Sha256Digest& digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string hex(64, '0');
    for (std::size_t i = 0; i < digest.size(); ++i) {
        hex[i * 2] = digits[digest[i] >> 4];
        hex[i * 2 + 1] = digits[digest[i] & 15];
    }
    return hex;
}
Sha256Digest sha256Stream(std::istream& input) {
    if (!input.good()) throw std::runtime_error("SHA-256 input is not readable");
    Sha256 hash;
    std::array<char, 64 * 1024> buffer;
    for (;;) {
        try { input.read(buffer.data(), static_cast<std::streamsize>(buffer.size())); }
        catch (const std::ios_base::failure&) {
            // With failbit exceptions enabled, a normal final short read throws.
            if (input.bad() || !input.eof()) throw std::runtime_error("SHA-256 input read failed");
        }
        // A real read error must never return a digest of only the readable prefix.
        if (input.bad() || (input.fail() && !input.eof()))
            throw std::runtime_error("SHA-256 input read failed");
        hash.update(std::string_view(buffer.data(), static_cast<std::size_t>(input.gcount())));
        if (input.eof()) break;
    }
    return hash.finish();
}
Sha256Digest sha256File(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) throw std::runtime_error("SHA-256 file open failed");
    return sha256Stream(input);
}
} // namespace chunkharbor
