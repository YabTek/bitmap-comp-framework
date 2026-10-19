#pragma once
// The interface every bitmap representation in this project implements.
//
// A backend owns the whole dataset: build() hands it all bitmaps at once in the
// raw uncompressed form, and it converts them into its own representation.
// Queries then refer to bitmaps by index.
//
// query_and() / query_or() must do the work *in the backend's own
// representation* -- that is the number the benchmark is trying to measure. The
// result is returned as a QueryResult, which still holds the compressed form;
// converting it back to raw words (to_words(), used only for verification)
// happens after the timer has been stopped.
#include "common/binary.hpp"
#include "common/statistics.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace bench {

class QueryResult {
  public:
    virtual ~QueryResult() = default;

    // Number of set bits. Cheap for most representations; never timed.
    virtual uint64_t cardinality() const = 0;

    // Decode back to the raw uint64_t form, for comparison against the
    // reference backend under --verify=1. Never timed.
    virtual std::vector<uint64_t> to_words() const = 0;
    virtual size_t payload_bytes() const = 0;
    virtual RepresentationStats statistics() const { return {}; }
};

using QueryResultPtr = std::unique_ptr<QueryResult>;

class BitmapBackend {
  public:
    virtual ~BitmapBackend() = default;

    // Convert the dataset into this backend's representation. `bitmaps[i]` has
    // words_for_bits(num_bits) words.
    virtual void build(const std::vector<std::vector<uint64_t>> &bitmaps, uint64_t num_bits) = 0;

    // Intersection / union of the listed bitmaps, computed natively.
    // `ids` holds at least one index; indices are valid by construction.
    virtual QueryResultPtr query_and(const std::vector<size_t> &ids) const = 0;
    virtual QueryResultPtr query_or(const std::vector<size_t> &ids) const = 0;

    // Exact saved dataset size: native payloads plus the shared archive header
    // and per-bitmap lengths. Excludes allocator slack and resident objects.
    // serialize().size() must equal this value. The compression ratio is
    // raw.serialized_bytes() / backend.serialized_bytes().
    virtual size_t serialized_bytes() const = 0;
    virtual size_t payload_bytes() const = 0;
    virtual RepresentationStats statistics() const { return {}; }

    // Exact archive bytes, including logical length and per-bitmap framing.
    virtual Bytes serialize() const = 0;
    virtual void deserialize(const Bytes &bytes) = 0;

    virtual const char *name() const = 0;
};

} // namespace bench
