#include "chunkharbor/client_core/SourceSnapshot.h"
#include <algorithm>
namespace chunkharbor::client {
std::optional<protocol::ManifestPage> ManifestReader::next() {
    const auto &m = source_.metadata;
    if (!m.chunkSize || m.chunkSize > protocol::MaxChunkDataSize || m.size > 64ULL * 1024 * 1024)
        throw std::runtime_error("invalid source geometry");
    const auto blocks = (m.size + m.chunkSize - 1) / m.chunkSize;
    if (blocks > 65536)
        throw std::runtime_error("too many source blocks");
    const auto first = page_ * 512U;
    if (first >= blocks)
        return {};
    protocol::ManifestPage page{session_, page_++, first, {}};
    for (std::uint32_t i = first; i < std::min<std::uint64_t>(first + 512, blocks); ++i) {
        auto offset = std::uint64_t(i) * m.chunkSize;
        auto length = std::min<std::uint64_t>(m.chunkSize, m.size - offset);
        auto bytes = source_.read(offset, static_cast<std::size_t>(length));
        if (bytes.size() != length)
            throw std::runtime_error("short source read");
        page.hashes.push_back(source_.hash(bytes));
    }
    return page;
}
} // namespace chunkharbor::client
