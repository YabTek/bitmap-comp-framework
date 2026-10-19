#pragma once
// WAH (Word-Aligned Hybrid) run-length compression, 64-bit variant.
//
// The bit sequence is cut into groups of 63 bits. Each 64-bit code word is
// either
//
//   literal: bit 63 = 0, bits 0..62 = the 63 bits of one group verbatim;
//   fill:    bit 63 = 1, bit 62 = the repeated bit value,
//            bits 0..61 = how many consecutive groups are all-0 or all-1.
//
// So a long run of equal bits costs one word regardless of its length, while
// anything irregular costs one word per 63 bits -- slightly *more* than the
// uncompressed 64 bits per 64 bits. AND/OR run directly over the two compressed
// streams: at each step the two readers agree on how many groups they can
// consume at once (min of the two remaining run lengths), so a query over long
// runs touches few words. That is the whole point of the representation, and
// the reason its query time depends on run structure rather than on
// cardinality.
#include "backends/bitmap_backend.hpp"

namespace bench {

class WahBackend : public BitmapBackend {
  public:
    void build(const std::vector<std::vector<uint64_t>> &bitmaps, uint64_t num_bits) override;
    QueryResultPtr query_and(const std::vector<size_t> &ids) const override;
    QueryResultPtr query_or(const std::vector<size_t> &ids) const override;
    size_t serialized_bytes() const override;
    size_t payload_bytes() const override;
    RepresentationStats statistics() const override;
    Bytes serialize() const override;
    void deserialize(const Bytes &bytes) override;
    const char *name() const override { return "wah"; }

  private:
    // One compressed code-word stream per bitmap.
    std::vector<std::vector<uint64_t>> compressed_;
    uint64_t num_bits_ = 0;
};

} // namespace bench
