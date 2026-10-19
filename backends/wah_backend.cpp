#include "backends/wah_backend.hpp"

#include "backends/archive.hpp"
#include "common/bits.hpp"

#include <algorithm>

namespace bench {

namespace {

constexpr uint64_t kGroupBits = 63;
constexpr uint64_t kMask63 = (1ULL << 63) - 1ULL;    // payload of a literal
constexpr uint64_t kFillFlag = 1ULL << 63;           // set => fill word
constexpr uint64_t kFillOne = 1ULL << 62;            // fill of ones, not zeros
constexpr uint64_t kCountMask = (1ULL << 62) - 1ULL; // fill length, in groups

uint64_t groups_for_bits(uint64_t num_bits) { return (num_bits + kGroupBits - 1) / kGroupBits; }

// Read the 63 bits starting at `start` out of a raw bitmap. Bits past the end
// of the bitmap read as 0, which is exactly what the padding of the last group
// should be.
uint64_t read_group(const std::vector<uint64_t> &raw, uint64_t start) {
    const size_t wi = static_cast<size_t>(start / 64);
    const size_t off = static_cast<size_t>(start % 64);
    if (wi >= raw.size())
        return 0;
    uint64_t v = raw[wi] >> off;
    if (off != 0 && wi + 1 < raw.size())
        v |= raw[wi + 1] << (64 - off);
    return v & kMask63;
}

void write_group(std::vector<uint64_t> &raw, uint64_t start, uint64_t value) {
    const size_t wi = static_cast<size_t>(start / 64);
    const size_t off = static_cast<size_t>(start % 64);
    if (wi >= raw.size())
        return;
    raw[wi] |= value << off;
    if (off != 0 && wi + 1 < raw.size())
        raw[wi + 1] |= value >> (64 - off);
}

// Appends groups to a code-word stream, merging adjacent fills of equal value.
class Builder {
  public:
    void append(uint64_t value, uint64_t count) {
        if (count == 0)
            return;
        if (value != 0ULL && value != kMask63) {
            // Irregular group: one literal word per group. `count` is 1 unless
            // both operands happened to repeat the same irregular literal,
            // which the encoding cannot express more compactly.
            for (uint64_t i = 0; i < count; ++i)
                words_.push_back(value);
            return;
        }
        const uint64_t head = kFillFlag | (value ? kFillOne : 0ULL);
        if (!words_.empty() && (words_.back() & kFillFlag) != 0 &&
            (words_.back() & kFillOne) == (head & kFillOne)) {
            words_.back() += count; // extend the run in place
            return;
        }
        words_.push_back(head | count);
    }

    std::vector<uint64_t> take() { return std::move(words_); }

  private:
    std::vector<uint64_t> words_;
};

// Walks a code-word stream as a sequence of (group value, repeat count) runs.
class Reader {
  public:
    explicit Reader(const std::vector<uint64_t> &words) : words_(words) { load(); }

    bool done() const { return remaining_ == 0; }
    uint64_t value() const { return value_; }
    uint64_t remaining() const { return remaining_; }

    void advance(uint64_t n) {
        remaining_ -= n;
        if (remaining_ == 0)
            load();
    }

  private:
    void load() {
        if (index_ >= words_.size()) {
            value_ = 0;
            remaining_ = 0;
            return;
        }
        const uint64_t w = words_[index_++];
        if (w & kFillFlag) {
            value_ = (w & kFillOne) ? kMask63 : 0ULL;
            remaining_ = w & kCountMask;
        } else {
            value_ = w;
            remaining_ = 1;
        }
    }

    const std::vector<uint64_t> &words_;
    size_t index_ = 0;
    uint64_t value_ = 0;
    uint64_t remaining_ = 0;
};

std::vector<uint64_t> encode(const std::vector<uint64_t> &raw, uint64_t num_bits) {
    Builder b;
    const uint64_t groups = groups_for_bits(num_bits);
    for (uint64_t g = 0; g < groups; ++g)
        b.append(read_group(raw, g * kGroupBits), 1);
    return b.take();
}

std::vector<uint64_t> decode(const std::vector<uint64_t> &compressed, uint64_t num_bits) {
    std::vector<uint64_t> raw(words_for_bits(num_bits), 0);
    uint64_t g = 0;
    for (Reader r(compressed); !r.done();) {
        const uint64_t n = r.remaining();
        if (r.value() != 0ULL)
            for (uint64_t i = 0; i < n; ++i)
                write_group(raw, (g + i) * kGroupBits, r.value());
        g += n;
        r.advance(n);
    }
    mask_tail(raw, num_bits);
    return raw;
}

// The core of the backend: one pass over both compressed streams, no
// decompression. Both streams describe the same number of groups, so they run
// out together.
std::vector<uint64_t> merge(const std::vector<uint64_t> &a, const std::vector<uint64_t> &b,
                            bool is_and) {
    Builder out;
    Reader ra(a), rb(b);
    while (!ra.done() && !rb.done()) {
        const uint64_t n = std::min(ra.remaining(), rb.remaining());
        const uint64_t v = is_and ? (ra.value() & rb.value()) : (ra.value() | rb.value());
        out.append(v, n);
        ra.advance(n);
        rb.advance(n);
    }
    return out.take();
}

class WahResult : public QueryResult {
  public:
    WahResult(std::vector<uint64_t> compressed, uint64_t num_bits)
        : compressed_(std::move(compressed)), num_bits_(num_bits) {}

    uint64_t cardinality() const override {
        // Counted straight off the code words. A ones-fill can never cover
        // padding bits (the padding is zero, so its group is not all-ones),
        // hence 63 bits per filled group is exact.
        uint64_t c = 0;
        for (uint64_t w : compressed_) {
            if (w & kFillFlag) {
                if (w & kFillOne)
                    c += kGroupBits * (w & kCountMask);
            } else {
                c += static_cast<uint64_t>(popcount64(w));
            }
        }
        return c;
    }

    std::vector<uint64_t> to_words() const override { return decode(compressed_, num_bits_); }
    size_t payload_bytes() const override { return compressed_.size() * sizeof(uint64_t); }
    RepresentationStats statistics() const override {
        RepresentationStats s;
        for (auto w : compressed_) {
            if (w & kFillFlag)
                ++s.fill_units;
            else
                ++s.literal_units;
        }
        return s;
    }

  private:
    std::vector<uint64_t> compressed_;
    uint64_t num_bits_;
};

} // namespace

void WahBackend::build(const std::vector<std::vector<uint64_t>> &bitmaps, uint64_t num_bits) {
    validate_inputs(bitmaps, num_bits);
    num_bits_ = num_bits;
    compressed_.clear();
    compressed_.reserve(bitmaps.size());
    for (const auto &raw : bitmaps)
        compressed_.push_back(encode(raw, num_bits));
}

QueryResultPtr WahBackend::query_and(const std::vector<size_t> &ids) const {
    validate_ids(ids, compressed_.size());
    std::vector<uint64_t> acc = compressed_[ids[0]];
    for (size_t k = 1; k < ids.size(); ++k)
        acc = merge(acc, compressed_[ids[k]], /*is_and=*/true);
    return std::make_unique<WahResult>(std::move(acc), num_bits_);
}

QueryResultPtr WahBackend::query_or(const std::vector<size_t> &ids) const {
    validate_ids(ids, compressed_.size());
    std::vector<uint64_t> acc = compressed_[ids[0]];
    for (size_t k = 1; k < ids.size(); ++k)
        acc = merge(acc, compressed_[ids[k]], /*is_and=*/false);
    return std::make_unique<WahResult>(std::move(acc), num_bits_);
}

// Native payloads are code-word streams; saved sizes also include the shared
// logical-length header and per-bitmap lengths.
size_t WahBackend::serialized_bytes() const {
    size_t bytes = 32 + 8 * compressed_.size();
    for (const auto &c : compressed_)
        bytes += c.size() * sizeof(uint64_t);
    return bytes;
}
size_t WahBackend::payload_bytes() const {
    size_t result = 0;
    for (const auto &b : compressed_)
        result += b.size() * 8;
    return result;
}
RepresentationStats WahBackend::statistics() const {
    RepresentationStats s;
    for (const auto &b : compressed_)
        for (auto w : b) {
            if (w & kFillFlag)
                ++s.fill_units;
            else
                ++s.literal_units;
        }
    return s;
}

Bytes WahBackend::serialize() const {
    DatasetArchive a{format_id(name()), num_bits_, {}};
    for (const auto &b : compressed_) {
        Bytes p;
        for (auto word : b)
            put_le(p, word, 8);
        a.payloads.push_back(std::move(p));
    }
    return pack_archive(a);
}
void WahBackend::deserialize(const Bytes &bytes) {
    const auto a = unpack_archive(bytes, format_id(name()));
    std::vector<std::vector<uint64_t>> next;
    const uint64_t expected = groups_for_bits(a.num_bits);
    for (const auto &p : a.payloads) {
        if (p.size() % 8)
            throw std::runtime_error("truncated WAH code word");
        ByteReader in(p);
        std::vector<uint64_t> b;
        uint64_t seen = 0;
        while (in.remaining()) {
            const uint64_t word = in.get(8);
            const uint64_t count = word & kFillFlag ? word & kCountMask : 1;
            if (!count || count > expected - seen)
                throw std::runtime_error("invalid WAH fill length");
            seen += count;
            if (seen == expected && a.num_bits % 63) {
                const uint64_t value = word & kFillFlag ? (word & kFillOne ? kMask63 : 0) : word;
                if (value >> (a.num_bits % 63))
                    throw std::runtime_error("nonzero WAH padding");
            }
            b.push_back(word);
        }
        if (seen != expected)
            throw std::runtime_error("WAH length mismatch");
        next.push_back(std::move(b));
    }
    compressed_ = std::move(next);
    num_bits_ = a.num_bits;
}

} // namespace bench
