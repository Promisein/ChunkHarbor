#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace chunkharbor {

// Single-owner byte buffer; synchronize externally if used across threads.
// Invariant: 0 <= readIndex_ <= writeIndex_ <= storage_.size() <= maxCapacity_.
class Buffer {
public:
    explicit Buffer(std::size_t initialCapacity = 1024,
                    std::size_t maxCapacity = 4 * 1024 * 1024);
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    Buffer(Buffer&&) = delete;
    Buffer& operator=(Buffer&&) = delete;

    std::size_t readableBytes() const noexcept { return writeIndex_ - readIndex_; }
    std::size_t writableBytes() const noexcept { return storage_.size() - writeIndex_; }
    // Usable storage, excluding allocator overhead (not vector::capacity()).
    std::size_t capacity() const noexcept { return storage_.size(); }
    std::size_t maxCapacity() const noexcept { return maxCapacity_; }
    std::string_view readableView() const noexcept;

    void append(std::string_view bytes); // Supports views into this buffer.
    void consume(std::size_t length);    // Throws out_of_range; never truncates silently.
    std::string read(std::size_t length);
    std::string readAll() { return read(readableBytes()); }

    // TcpConnection may recv directly into this storage, then commit only bytes received.
    // Any mutation may invalidate previously obtained pointers/views. Never retain them
    // across ensureWritable/append/consume/read/commitWrite or a later event-loop turn.
    void ensureWritable(std::size_t length); // length_error on limit, bad_alloc on allocation.
    char* writableData() noexcept;
    void commitWrite(std::size_t length);

private:
    std::vector<char> storage_;
    const std::size_t maxCapacity_;
    std::size_t readIndex_ = 0;
    std::size_t writeIndex_ = 0;
};

} // namespace chunkharbor
