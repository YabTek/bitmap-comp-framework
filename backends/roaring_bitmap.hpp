#pragma once
// A compact, self-contained Roaring bitmap.
//
// Portable scalar implementation of the Roaring design, with official portable
// serialization. It is not CRoaring: implementation choices affect both query
// time and container selection; see docs/formats.md before interpreting
// results.
//
// Design. The universe is split into chunks of 2^16 bits. Only non-empty chunks
// are stored, each as a container that picks whichever of three encodings is
// smallest for the bits it holds:
//
//   Array  - sorted 16-bit offsets;      2 bytes per set bit
//   Bitset - 1024 uncompressed words;    8192 bytes flat
//   Run    - sorted [start, last] pairs; 2 + 4 bytes per run list
//
// AND and OR are performed container by container, and each pair of encodings
// has its own algorithm, so the cost of a query depends on which encodings the
// data selected. After every operation the result container re-picks its
// smallest encoding (see Container::optimize).
#include "common/binary.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace bench {
namespace roaring {

constexpr size_t kChunkBits = 1u << 16;         // bits per chunk
constexpr size_t kChunkWords = kChunkBits / 64; // = 1024

// Inclusive interval of 16-bit offsets within a chunk.
struct Run {
    uint16_t start;
    uint16_t last;
};

class Container {
  public:
    enum class Kind { Array, Bitset, Run };

    // Only the vector matching `kind` is populated. Keeping all three members
    // in one struct costs some resident memory but keeps the code readable;
    // it does not affect serialized_bytes(), which is what the compression
    // ratio is computed from.
    Kind kind = Kind::Array;
    std::vector<uint16_t> values; // Kind::Array
    std::vector<uint64_t> bits;   // Kind::Bitset, always kChunkWords long
    std::vector<Run> runs;        // Kind::Run

    static Container from_bitset(std::vector<uint64_t> words);

    uint64_t cardinality() const;
    size_t serialized_bytes() const;

    bool empty() const;

    std::vector<uint64_t> to_bitset() const;

    // Re-encode into whichever kind is smallest for the current contents.
    void optimize();
};

Container container_and(const Container &a, const Container &b);
Container container_or(const Container &a, const Container &b);

struct Chunk {
    uint16_t key; // high 16 bits of every value in this chunk
    Container container;
};

class Bitmap {
  public:
    // `raw` is the uncompressed form: words_for_bits(num_bits) words.
    static Bitmap from_words(const std::vector<uint64_t> &raw, uint64_t num_bits);

    std::vector<uint64_t> to_words(uint64_t num_bits) const;
    uint64_t cardinality() const;

    // Exact official portable serialization size, including its cookie,
    // descriptors, optional run bitmap and offsets, and container payloads.
    size_t serialized_bytes() const;
    Bytes serialize() const;
    static Bitmap deserialize(const Bytes &payload);

    const std::vector<Chunk> &chunks() const { return chunks_; }

  private:
    std::vector<Chunk> chunks_; // sorted by key, never empty containers

    friend Bitmap bitmap_and(const Bitmap &a, const Bitmap &b);
    friend Bitmap bitmap_or(const Bitmap &a, const Bitmap &b);
};

Bitmap bitmap_and(const Bitmap &a, const Bitmap &b);
Bitmap bitmap_or(const Bitmap &a, const Bitmap &b);

} // namespace roaring
} // namespace bench
