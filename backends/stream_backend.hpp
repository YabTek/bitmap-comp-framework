#pragma once
#include "backends/bitmap_backend.hpp"
#include "backends/codecs.hpp"

namespace bench {
class StreamBackend final : public BitmapBackend {
  public:
    explicit StreamBackend(Codec codec, CodecOptions options = {})
        : codec_(codec), options_(options) {}
    void build(const std::vector<std::vector<uint64_t>> &bitmaps, uint64_t n) override;
    QueryResultPtr query_and(const std::vector<size_t> &ids) const override;
    QueryResultPtr query_or(const std::vector<size_t> &ids) const override;
    size_t serialized_bytes() const override;
    size_t payload_bytes() const override;
    RepresentationStats statistics() const override;
    Bytes serialize() const override;
    void deserialize(const Bytes &bytes) override;
    const char *name() const override { return codec_name(codec_); }

  private:
    QueryResultPtr query(const std::vector<size_t> &ids, bool is_and) const;
    Codec codec_;
    CodecOptions options_;
    uint64_t num_bits_ = 0;
    std::vector<Encoding> bitmaps_;
};
} // namespace bench
