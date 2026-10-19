#pragma once
// Explicit little-endian I/O. The benchmark's archive is versioned separately
// from the format-specific payloads; see docs/formats.md.
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace bench {
using Bytes = std::vector<uint8_t>;

inline void put_le(Bytes &out, uint64_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i)
        out.push_back(static_cast<uint8_t>(value >> (i * 8)));
}

class ByteReader {
  public:
    explicit ByteReader(const Bytes &data) : data_(data) {}
    size_t remaining() const { return data_.size() - pos_; }
    size_t position() const { return pos_; }
    uint64_t get(unsigned bytes) {
        if (bytes > 8 || bytes > remaining())
            throw std::runtime_error("truncated binary data");
        uint64_t v = 0;
        for (unsigned i = 0; i < bytes; ++i)
            v |= uint64_t(data_[pos_++]) << (8 * i);
        return v;
    }
    Bytes take(uint64_t length) {
        if (length > remaining())
            throw std::runtime_error("truncated binary payload");
        const auto n = static_cast<size_t>(length);
        Bytes result(data_.begin() + static_cast<std::ptrdiff_t>(pos_),
                     data_.begin() + static_cast<std::ptrdiff_t>(pos_ + n));
        pos_ += n;
        return result;
    }
    void require_end() const {
        if (remaining())
            throw std::runtime_error("unexpected trailing binary data");
    }

  private:
    const Bytes &data_;
    size_t pos_ = 0;
};

inline uint64_t low_mask(unsigned width) {
    return width == 64 ? ~uint64_t(0) : ((uint64_t(1) << width) - 1);
}
} // namespace bench
