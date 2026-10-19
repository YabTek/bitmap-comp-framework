#include "backends/roaring_backend.hpp"

#include "backends/archive.hpp"
#include "common/bits.hpp"
#include <stdexcept>

namespace bench {

namespace {
RepresentationStats stats_for(const roaring::Bitmap &b) {
    RepresentationStats s;
    for (const auto &c : b.chunks()) {
        if (c.container.kind == roaring::Container::Kind::Array)
            ++s.array_containers;
        else if (c.container.kind == roaring::Container::Kind::Bitset)
            ++s.bitmap_containers;
        else
            ++s.run_containers;
    }
    return s;
}

class RoaringResult : public QueryResult {
  public:
    RoaringResult(roaring::Bitmap bm, uint64_t num_bits)
        : bitmap_(std::move(bm)), num_bits_(num_bits) {}

    uint64_t cardinality() const override { return bitmap_.cardinality(); }
    std::vector<uint64_t> to_words() const override { return bitmap_.to_words(num_bits_); }
    size_t payload_bytes() const override { return bitmap_.serialized_bytes(); }
    RepresentationStats statistics() const override { return stats_for(bitmap_); }

  private:
    roaring::Bitmap bitmap_;
    uint64_t num_bits_;
};

} // namespace

void RoaringBackend::build(const std::vector<std::vector<uint64_t>> &bitmaps, uint64_t num_bits) {
    validate_inputs(bitmaps, num_bits);
    // Chunk keys are 16-bit, so the addressable universe is 2^32 bits.
    if (num_bits > (1ULL << 32))
        throw std::runtime_error("roaring backend: num_bits exceeds 2^32");
    num_bits_ = num_bits;
    bitmaps_.clear();
    bitmaps_.reserve(bitmaps.size());
    for (const auto &raw : bitmaps)
        bitmaps_.push_back(roaring::Bitmap::from_words(raw, num_bits));
}

QueryResultPtr RoaringBackend::query_and(const std::vector<size_t> &ids) const {
    validate_ids(ids, bitmaps_.size());
    roaring::Bitmap acc = bitmaps_[ids[0]];
    for (size_t k = 1; k < ids.size(); ++k)
        acc = roaring::bitmap_and(acc, bitmaps_[ids[k]]);
    return std::make_unique<RoaringResult>(std::move(acc), num_bits_);
}

QueryResultPtr RoaringBackend::query_or(const std::vector<size_t> &ids) const {
    validate_ids(ids, bitmaps_.size());
    roaring::Bitmap acc = bitmaps_[ids[0]];
    for (size_t k = 1; k < ids.size(); ++k)
        acc = roaring::bitmap_or(acc, bitmaps_[ids[k]]);
    return std::make_unique<RoaringResult>(std::move(acc), num_bits_);
}

size_t RoaringBackend::serialized_bytes() const {
    size_t bytes = 32 + 8 * bitmaps_.size();
    for (const auto &bm : bitmaps_)
        bytes += bm.serialized_bytes();
    return bytes;
}
size_t RoaringBackend::payload_bytes() const {
    size_t result = 0;
    for (const auto &b : bitmaps_)
        result += b.serialized_bytes();
    return result;
}
RepresentationStats RoaringBackend::statistics() const {
    RepresentationStats s;
    for (const auto &b : bitmaps_)
        s += stats_for(b);
    return s;
}

Bytes RoaringBackend::serialize() const {
    DatasetArchive a{format_id(name()), num_bits_, {}};
    for (const auto &b : bitmaps_)
        a.payloads.push_back(b.serialize());
    return pack_archive(a);
}
void RoaringBackend::deserialize(const Bytes &bytes) {
    auto a = unpack_archive(bytes, format_id(name()));
    std::vector<roaring::Bitmap> next;
    for (const auto &p : a.payloads) {
        auto b = roaring::Bitmap::deserialize(p);
        for (const auto &chunk : b.chunks()) {
            const uint64_t base = uint64_t(chunk.key) * roaring::kChunkBits;
            if (base >= a.num_bits)
                throw std::runtime_error("Roaring container beyond logical length");
            if (a.num_bits - base < roaring::kChunkBits) {
                const auto words = chunk.container.to_bitset();
                const uint64_t local = a.num_bits - base;
                for (uint64_t i = local / 64; i < words.size(); ++i) {
                    const uint64_t allowed = i == local / 64 && local % 64
                                                 ? low_mask(static_cast<unsigned>(local % 64))
                                                 : 0;
                    if (words[i] & ~allowed)
                        throw std::runtime_error("nonzero Roaring padding");
                }
            }
        }
        next.push_back(std::move(b));
    }
    bitmaps_ = std::move(next);
    num_bits_ = a.num_bits;
}

} // namespace bench
