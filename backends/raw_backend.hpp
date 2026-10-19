#pragma once
// Uncompressed baseline: one packed uint64_t array per bitmap.
//
// Two roles in this project:
//   1. the denominator of the compression ratio (num_bitmaps * num_bits / 8);
//   2. the correctness oracle -- under --verify=1 every other backend's result
//      is compared bit for bit against this one.
#include "backends/bitmap_backend.hpp"

namespace bench {

class RawBackend : public BitmapBackend {
  public:
    void build(const std::vector<std::vector<uint64_t>> &bitmaps, uint64_t num_bits) override;
    QueryResultPtr query_and(const std::vector<size_t> &ids) const override;
    QueryResultPtr query_or(const std::vector<size_t> &ids) const override;
    size_t serialized_bytes() const override;
    size_t payload_bytes() const override;
    Bytes serialize() const override;
    void deserialize(const Bytes &bytes) override;
    const char *name() const override { return "raw"; }

  private:
    std::vector<std::vector<uint64_t>> bitmaps_;
    uint64_t num_bits_ = 0;
    size_t num_words_ = 0;
};

} // namespace bench
