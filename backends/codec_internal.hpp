#pragma once
#include "backends/codecs.hpp"
#include "common/bits.hpp"
#include <algorithm>
#include <functional>

namespace bench::detail {
struct Cursor {
    RepresentationStats stats;
    virtual ~Cursor() = default;
    virtual bool next(Span &span) = 0;
};
struct Writer {
    virtual ~Writer() = default;
    virtual void append(uint64_t value, uint64_t count) = 0;
    virtual Encoding finish() = 0;
};
std::unique_ptr<Cursor> word_cursor(const Encoding &e);
std::unique_ptr<Writer> word_writer(Codec codec, uint64_t n, unsigned width);
std::unique_ptr<Cursor> byte_cursor(const Encoding &e);
std::unique_ptr<Writer> byte_writer(Codec codec, uint64_t n, unsigned groups);
inline uint64_t group_count(uint64_t n, unsigned width) { return n / width + (n % width != 0); }
inline uint64_t read_group(const std::vector<uint64_t> &words, uint64_t start, unsigned width) {
    const size_t i = static_cast<size_t>(start / 64);
    const unsigned off = static_cast<unsigned>(start % 64);
    if (i >= words.size())
        return 0;
    uint64_t value = words[i] >> off;
    if (off && width > 64 - off && i + 1 < words.size())
        value |= words[i + 1] << (64 - off);
    return value & low_mask(width);
}
inline void write_group(std::vector<uint64_t> &words, uint64_t start, unsigned width,
                        uint64_t value) {
    const size_t i = static_cast<size_t>(start / 64);
    const unsigned off = static_cast<unsigned>(start % 64);
    value &= low_mask(width);
    if (i >= words.size())
        return;
    words[i] |= value << off;
    if (off && width > 64 - off && i + 1 < words.size())
        words[i + 1] |= value >> (64 - off);
}
inline void set_le(Bytes &bytes, size_t pos, uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i)
        bytes.at(pos + i) = static_cast<uint8_t>(value >> (8 * i));
}
inline bool uniform(uint64_t v, unsigned w) { return v == 0 || v == low_mask(w); }
inline void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
} // namespace bench::detail
