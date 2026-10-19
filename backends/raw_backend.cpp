#include "backends/raw_backend.hpp"

#include "backends/archive.hpp"
#include "common/bits.hpp"

namespace bench {

namespace {

// A result already in raw form; to_words() copies the words without decoding.
class RawResult : public QueryResult {
  public:
    explicit RawResult(std::vector<uint64_t> words) : words_(std::move(words)) {}
    uint64_t cardinality() const override { return bench::cardinality(words_); }
    std::vector<uint64_t> to_words() const override { return words_; }
    size_t payload_bytes() const override { return words_.size() * sizeof(uint64_t); }

  private:
    std::vector<uint64_t> words_;
};

} // namespace

void RawBackend::build(const std::vector<std::vector<uint64_t>> &bitmaps, uint64_t num_bits) {
    validate_inputs(bitmaps, num_bits);
    bitmaps_ = bitmaps;
    num_bits_ = num_bits;
    num_words_ = words_for_bits(num_bits);
}

QueryResultPtr RawBackend::query_and(const std::vector<size_t> &ids) const {
    validate_ids(ids, bitmaps_.size());
    std::vector<uint64_t> result = bitmaps_[ids[0]];
    for (size_t k = 1; k < ids.size(); ++k) {
        const auto &b = bitmaps_[ids[k]];
        for (size_t i = 0; i < num_words_; ++i)
            result[i] &= b[i];
    }
    mask_tail(result, num_bits_);
    return std::make_unique<RawResult>(std::move(result));
}

QueryResultPtr RawBackend::query_or(const std::vector<size_t> &ids) const {
    validate_ids(ids, bitmaps_.size());
    std::vector<uint64_t> result(num_words_, 0);
    for (size_t id : ids) {
        const auto &b = bitmaps_[id];
        for (size_t i = 0; i < num_words_; ++i)
            result[i] |= b[i];
    }
    mask_tail(result, num_bits_);
    return std::make_unique<RawResult>(std::move(result));
}

// Saved Raw includes the packed arrays and the same archive framing used by
// the compressed backends.
size_t RawBackend::serialized_bytes() const {
    return 32 + bitmaps_.size() * (8 + num_words_ * sizeof(uint64_t));
}
size_t RawBackend::payload_bytes() const { return bitmaps_.size() * num_words_ * sizeof(uint64_t); }

Bytes RawBackend::serialize() const {
    DatasetArchive a{format_id(name()), num_bits_, {}};
    for (const auto &b : bitmaps_) {
        Bytes p;
        for (auto word : b)
            put_le(p, word, 8);
        a.payloads.push_back(std::move(p));
    }
    return pack_archive(a);
}
void RawBackend::deserialize(const Bytes &bytes) {
    const auto a = unpack_archive(bytes, format_id(name()));
    std::vector<std::vector<uint64_t>> next;
    const auto words = words_for_bits(a.num_bits);
    for (const auto &p : a.payloads) {
        if (p.size() != words * 8)
            throw std::runtime_error("raw archive bitmap size mismatch");
        ByteReader in(p);
        std::vector<uint64_t> b;
        while (in.remaining())
            b.push_back(in.get(8));
        next.push_back(std::move(b));
    }
    build(next, a.num_bits);
}

} // namespace bench
