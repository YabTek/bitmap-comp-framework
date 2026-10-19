#include "backends/stream_backend.hpp"
#include "backends/archive.hpp"

namespace bench {
namespace {
class EncodedResult final : public QueryResult {
  public:
    explicit EncodedResult(Encoding value) : value_(std::move(value)) {}
    uint64_t cardinality() const override { return encoding_cardinality(value_); }
    std::vector<uint64_t> to_words() const override { return decode_bitmap(value_); }
    size_t payload_bytes() const override { return value_.bytes.size(); }
    RepresentationStats statistics() const override { return encoding_statistics(value_); }

  private:
    Encoding value_;
};
} // namespace
void StreamBackend::build(const std::vector<std::vector<uint64_t>> &data, uint64_t n) {
    validate_inputs(data, n);
    std::vector<Encoding> next;
    next.reserve(data.size());
    for (const auto &b : data)
        next.push_back(encode_bitmap(codec_, b, n, options_));
    bitmaps_ = std::move(next);
    num_bits_ = n;
}
QueryResultPtr StreamBackend::query(const std::vector<size_t> &ids, bool is_and) const {
    validate_ids(ids, bitmaps_.size());
    Encoding acc = bitmaps_[ids[0]];
    for (size_t i = 1; i < ids.size(); ++i)
        acc = combine_bitmaps(acc, bitmaps_[ids[i]], is_and);
    return std::make_unique<EncodedResult>(std::move(acc));
}
QueryResultPtr StreamBackend::query_and(const std::vector<size_t> &ids) const {
    return query(ids, true);
}
QueryResultPtr StreamBackend::query_or(const std::vector<size_t> &ids) const {
    return query(ids, false);
}
size_t StreamBackend::serialized_bytes() const {
    size_t result = 32 + 8 * bitmaps_.size();
    for (const auto &b : bitmaps_)
        result += b.bytes.size();
    return result;
}
size_t StreamBackend::payload_bytes() const {
    size_t size = 0;
    for (const auto &b : bitmaps_)
        size += b.bytes.size();
    return size;
}
RepresentationStats StreamBackend::statistics() const {
    RepresentationStats result;
    for (const auto &b : bitmaps_)
        result += encoding_statistics(b);
    return result;
}
Bytes StreamBackend::serialize() const {
    DatasetArchive a{format_id(name()), num_bits_, {}};
    for (const auto &b : bitmaps_)
        a.payloads.push_back(b.bytes);
    return pack_archive(a);
}
void StreamBackend::deserialize(const Bytes &bytes) {
    auto a = unpack_archive(bytes, format_id(name()));
    std::vector<Encoding> next;
    for (const auto &p : a.payloads)
        next.push_back(load_encoding(codec_, a.num_bits, p));
    bitmaps_ = std::move(next);
    num_bits_ = a.num_bits;
}
} // namespace bench
