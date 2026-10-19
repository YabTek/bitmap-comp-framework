#pragma once
// Backend adapter around the Roaring bitmap in roaring_bitmap.hpp.
#include "backends/bitmap_backend.hpp"
#include "backends/roaring_bitmap.hpp"

namespace bench {

class RoaringBackend : public BitmapBackend {
  public:
    void build(const std::vector<std::vector<uint64_t>> &bitmaps, uint64_t num_bits) override;
    QueryResultPtr query_and(const std::vector<size_t> &ids) const override;
    QueryResultPtr query_or(const std::vector<size_t> &ids) const override;
    size_t serialized_bytes() const override;
    size_t payload_bytes() const override;
    RepresentationStats statistics() const override;
    Bytes serialize() const override;
    void deserialize(const Bytes &bytes) override;
    const char *name() const override { return "roaring"; }

  private:
    std::vector<roaring::Bitmap> bitmaps_;
    uint64_t num_bits_ = 0;
};

} // namespace bench
