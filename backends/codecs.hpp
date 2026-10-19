#pragma once
// Scalar reference codecs. No external dependency, SIMD, or claim to reproduce
// paper benchmark timings. Logical operations consume compressed spans, not
// full uncompressed inputs (SBH instead uses bounded super-bucket buffers).
#include "common/binary.hpp"
#include "common/statistics.hpp"
#include <memory>
#include <string>

namespace bench {
enum class Codec { Ewah, Concise, Plwah, Val, Sbh, Bbc };
struct CodecOptions {
    unsigned val_segment_bits = 15; // 0 = choose smallest payload among 15/30/60
    unsigned sbh_groups = 128;      // seven original bits per group
};
struct Encoding {
    Codec codec;
    uint64_t num_bits = 0;
    unsigned width = 0;
    unsigned sbh_groups = 128;
    Bytes bytes;
};
struct Span {
    uint64_t value = 0, count = 0;
};

Encoding encode_bitmap(Codec codec, const std::vector<uint64_t> &raw, uint64_t n,
                       CodecOptions options = {});
Encoding load_encoding(Codec codec, uint64_t n, const Bytes &payload);
std::vector<uint64_t> decode_bitmap(const Encoding &encoding);
uint64_t encoding_cardinality(const Encoding &encoding);
RepresentationStats encoding_statistics(const Encoding &encoding);
Encoding combine_bitmaps(const Encoding &a, const Encoding &b, bool is_and);
const char *codec_name(Codec codec);
// Public for small executable examples and conformance tests.
std::vector<Span> inspect_spans(const Encoding &encoding);
} // namespace bench
