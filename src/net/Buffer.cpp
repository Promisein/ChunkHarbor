#include "chunkharbor/net/Buffer.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <stdexcept>

namespace chunkharbor {

Buffer::Buffer(std::size_t initialCapacity, std::size_t maxCapacity)
    : maxCapacity_(maxCapacity) {
    if (initialCapacity > maxCapacity) throw std::invalid_argument("initial capacity exceeds limit");
    if (maxCapacity > storage_.max_size()) throw std::length_error("buffer limit exceeds vector limit");
    storage_.resize(initialCapacity);
}

std::string_view Buffer::readableView() const noexcept {
    if (readableBytes() == 0) return {};
    return {storage_.data() + readIndex_, readableBytes()};
}

char* Buffer::writableData() noexcept {
    return storage_.empty() ? nullptr : storage_.data() + writeIndex_;
}

void Buffer::ensureWritable(std::size_t length) {
    const auto readable = readableBytes();
    // Subtraction avoids overflow even for a hostile SIZE_MAX request.
    if (length > maxCapacity_ - readable) throw std::length_error("buffer capacity limit exceeded");
    if (length <= writableBytes()) return;
    if (length <= storage_.size() - readable) {
        if (readable != 0) std::memmove(storage_.data(), storage_.data() + readIndex_, readable);
    } else {
        const auto grown = capacity() > maxCapacity_ / 2 ? maxCapacity_ : capacity() * 2;
        std::vector<char> replacement(std::max(readable + length, grown));
        if (readable != 0) std::memcpy(replacement.data(), storage_.data() + readIndex_, readable);
        storage_.swap(replacement);
    }
    readIndex_ = 0;
    writeIndex_ = readable;
}

void Buffer::append(std::string_view bytes) {
    if (bytes.empty()) return;
    if (bytes.size() > maxCapacity_ - readableBytes())
        throw std::length_error("buffer capacity limit exceeded");
    // A self view could be invalidated by compaction or reallocation. Snapshot only
    // overlapping input; std::less provides a total ordering for unrelated pointers.
    std::string snapshot;
    const std::less<const char*> less;
    if (!storage_.empty() && less(bytes.data(), storage_.data() + storage_.size()) &&
        less(storage_.data(), bytes.data() + bytes.size())) {
        snapshot.assign(bytes.data(), bytes.size());
        bytes = snapshot;
    }
    ensureWritable(bytes.size());
    std::memcpy(writableData(), bytes.data(), bytes.size());
    writeIndex_ += bytes.size();
}

void Buffer::consume(std::size_t length) {
    if (length > readableBytes()) throw std::out_of_range("consume exceeds readable bytes");
    readIndex_ += length;
    if (readIndex_ == writeIndex_) readIndex_ = writeIndex_ = 0;
}

std::string Buffer::read(std::size_t length) {
    if (length > readableBytes()) throw std::out_of_range("read exceeds readable bytes");
    if (length == 0) return {};
    std::string result(storage_.data() + readIndex_, length);
    consume(length);
    return result;
}

void Buffer::commitWrite(std::size_t length) {
    if (length > writableBytes()) throw std::out_of_range("write exceeds reserved space");
    writeIndex_ += length;
}

} // namespace chunkharbor
