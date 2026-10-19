#include "backends/roaring_bitmap.hpp"

#include "common/bits.hpp"

#include <algorithm>

namespace bench {
namespace roaring {

namespace {

size_t count_runs_in_bitset(const std::vector<uint64_t> &bits) {
    // A run starts at every 0 -> 1 transition, counting the bit before the
    // first word as 0.
    size_t runs = 0;
    uint64_t carry = 0;
    for (uint64_t w : bits) {
        runs += static_cast<size_t>(popcount64(w & ~((w << 1) | carry)));
        carry = w >> 63;
    }
    return runs;
}

size_t count_runs_in_array(const std::vector<uint16_t> &values) {
    if (values.empty())
        return 0;
    size_t runs = 1;
    for (size_t i = 1; i < values.size(); ++i)
        if (values[i] != static_cast<uint16_t>(values[i - 1] + 1))
            ++runs;
    return runs;
}

std::vector<uint16_t> bitset_to_array(const std::vector<uint64_t> &bits) {
    std::vector<uint16_t> out;
    for (size_t w = 0; w < bits.size(); ++w) {
        uint64_t word = bits[w];
        while (word) {
            const int b = ctz64(word);
            out.push_back(static_cast<uint16_t>(w * 64 + static_cast<size_t>(b)));
            word &= word - 1;
        }
    }
    return out;
}

std::vector<Run> bitset_to_runs(const std::vector<uint64_t> &bits) {
    std::vector<Run> out;
    size_t i = 0;
    const size_t n = bits.size() * 64;
    while (i < n) {
        if (!((bits[i / 64] >> (i % 64)) & 1ULL)) {
            ++i;
            continue;
        }
        const size_t start = i;
        while (i + 1 < n && ((bits[(i + 1) / 64] >> ((i + 1) % 64)) & 1ULL))
            ++i;
        out.push_back({static_cast<uint16_t>(start), static_cast<uint16_t>(i)});
        ++i;
    }
    return out;
}

// start/last are widened to 32 bits so that last == 65535 terminates the loop
// instead of wrapping around.
void set_range(std::vector<uint64_t> &bits, uint32_t start, uint32_t last) {
    for (uint32_t v = start; v <= last; ++v)
        bits[v / 64] |= (1ULL << (v % 64));
}

} // namespace

// ---------------------------------------------------------------------------
// Container
// ---------------------------------------------------------------------------

Container Container::from_bitset(std::vector<uint64_t> words) {
    Container c;
    c.kind = Kind::Bitset;
    c.bits = std::move(words);
    c.bits.resize(kChunkWords, 0);
    c.optimize();
    return c;
}

uint64_t Container::cardinality() const {
    switch (kind) {
    case Kind::Array:
        return values.size();
    case Kind::Bitset:
        return bench::cardinality(bits);
    case Kind::Run: {
        uint64_t c = 0;
        for (const Run &r : runs)
            c += static_cast<uint64_t>(r.last) - r.start + 1;
        return c;
    }
    }
    return 0;
}

size_t Container::serialized_bytes() const {
    switch (kind) {
    case Kind::Array:
        return 2 * values.size();
    case Kind::Bitset:
        return kChunkWords * sizeof(uint64_t); // 8192
    case Kind::Run:
        return 2 + 4 * runs.size(); // count + pairs
    }
    return 0;
}

bool Container::empty() const {
    switch (kind) {
    case Kind::Array:
        return values.empty();
    case Kind::Bitset:
        return cardinality() == 0;
    case Kind::Run:
        return runs.empty();
    }
    return true;
}

std::vector<uint64_t> Container::to_bitset() const {
    if (kind == Kind::Bitset)
        return bits;
    std::vector<uint64_t> out(kChunkWords, 0);
    if (kind == Kind::Array) {
        for (uint16_t v : values)
            out[v / 64] |= (1ULL << (v % 64));
    } else {
        for (const Run &r : runs)
            set_range(out, r.start, r.last);
    }
    return out;
}

void Container::optimize() {
    const uint64_t card = cardinality();
    if (card == 0) {
        kind = Kind::Array;
        values.clear();
        bits.clear();
        runs.clear();
        return;
    }

    const size_t nruns = (kind == Kind::Array) ? count_runs_in_array(values)
                         : (kind == Kind::Run) ? runs.size()
                                               : count_runs_in_bitset(bits);

    const size_t array_bytes = 2 * static_cast<size_t>(card);
    const size_t bitset_bytes = kChunkWords * sizeof(uint64_t);
    const size_t run_bytes = 2 + 4 * nruns;

    Kind best = Kind::Array;
    size_t best_bytes = array_bytes;
    if (run_bytes < best_bytes) {
        best = Kind::Run;
        best_bytes = run_bytes;
    }
    if (bitset_bytes < best_bytes) {
        best = Kind::Bitset;
    }
    if (best == kind)
        return;

    // Convert through the bitset form, which every kind can produce cheaply.
    const std::vector<uint64_t> expanded = to_bitset();
    values.clear();
    runs.clear();
    bits.clear();
    kind = best;
    switch (best) {
    case Kind::Array:
        values = bitset_to_array(expanded);
        break;
    case Kind::Run:
        runs = bitset_to_runs(expanded);
        break;
    case Kind::Bitset:
        bits = expanded;
        break;
    }
}

// ---------------------------------------------------------------------------
// Container operations
//
// Only the pairs that carry the interesting behaviour have a dedicated
// algorithm: array-array, array-bitset, bitset-bitset and run-run. A mixed pair
// involving a run container falls back to expanding both sides to a bitset and
// operating there. That shortcut is deliberate -- it keeps the code short --
// but it means a query mixing run-encoded and array-encoded chunks pays for the
// expansion. CRoaring implements all nine pairs natively.
// ---------------------------------------------------------------------------

namespace {

Container array_and_array(const std::vector<uint16_t> &a, const std::vector<uint16_t> &b) {
    Container out;
    out.kind = Container::Kind::Array;
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i] < b[j])
            ++i;
        else if (b[j] < a[i])
            ++j;
        else {
            out.values.push_back(a[i]);
            ++i;
            ++j;
        }
    }
    return out;
}

Container array_and_bitset(const std::vector<uint16_t> &a, const std::vector<uint64_t> &b) {
    Container out;
    out.kind = Container::Kind::Array;
    for (uint16_t v : a)
        if ((b[v / 64] >> (v % 64)) & 1ULL)
            out.values.push_back(v);
    return out;
}

Container array_or_array(const std::vector<uint16_t> &a, const std::vector<uint16_t> &b) {
    Container out;
    out.kind = Container::Kind::Array;
    out.values.reserve(a.size() + b.size());
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i] < b[j])
            out.values.push_back(a[i++]);
        else if (b[j] < a[i])
            out.values.push_back(b[j++]);
        else {
            out.values.push_back(a[i]);
            ++i;
            ++j;
        }
    }
    while (i < a.size())
        out.values.push_back(a[i++]);
    while (j < b.size())
        out.values.push_back(b[j++]);
    return out;
}

Container run_and_run(const std::vector<Run> &a, const std::vector<Run> &b) {
    Container out;
    out.kind = Container::Kind::Run;
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const uint16_t start = std::max(a[i].start, b[j].start);
        const uint16_t last = std::min(a[i].last, b[j].last);
        if (start <= last)
            out.runs.push_back({start, last});
        if (a[i].last < b[j].last)
            ++i;
        else
            ++j;
    }
    return out;
}

Container run_or_run(const std::vector<Run> &a, const std::vector<Run> &b) {
    Container out;
    out.kind = Container::Kind::Run;
    size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        Run next;
        if (j >= b.size() || (i < a.size() && a[i].start <= b[j].start))
            next = a[i++];
        else
            next = b[j++];
        // Coalesce with the previous interval when they overlap or touch.
        if (!out.runs.empty() &&
            static_cast<int32_t>(next.start) <= static_cast<int32_t>(out.runs.back().last) + 1) {
            out.runs.back().last = std::max(out.runs.back().last, next.last);
        } else {
            out.runs.push_back(next);
        }
    }
    return out;
}

Container bitset_op(const std::vector<uint64_t> &a, const std::vector<uint64_t> &b, bool is_and) {
    Container out;
    out.kind = Container::Kind::Bitset;
    out.bits.resize(kChunkWords);
    for (size_t i = 0; i < kChunkWords; ++i)
        out.bits[i] = is_and ? (a[i] & b[i]) : (a[i] | b[i]);
    return out;
}

} // namespace

Container container_and(const Container &a, const Container &b) {
    using K = Container::Kind;
    Container out;
    if (a.kind == K::Array && b.kind == K::Array) {
        out = array_and_array(a.values, b.values);
    } else if (a.kind == K::Array && b.kind == K::Bitset) {
        out = array_and_bitset(a.values, b.bits);
    } else if (a.kind == K::Bitset && b.kind == K::Array) {
        out = array_and_bitset(b.values, a.bits);
    } else if (a.kind == K::Bitset && b.kind == K::Bitset) {
        out = bitset_op(a.bits, b.bits, /*is_and=*/true);
    } else if (a.kind == K::Run && b.kind == K::Run) {
        out = run_and_run(a.runs, b.runs);
    } else {
        out = bitset_op(a.to_bitset(), b.to_bitset(), /*is_and=*/true);
    }
    out.optimize();
    return out;
}

Container container_or(const Container &a, const Container &b) {
    using K = Container::Kind;
    Container out;
    if (a.kind == K::Array && b.kind == K::Array) {
        out = array_or_array(a.values, b.values);
    } else if (a.kind == K::Bitset && b.kind == K::Bitset) {
        out = bitset_op(a.bits, b.bits, /*is_and=*/false);
    } else if (a.kind == K::Run && b.kind == K::Run) {
        out = run_or_run(a.runs, b.runs);
    } else {
        out = bitset_op(a.to_bitset(), b.to_bitset(), /*is_and=*/false);
    }
    out.optimize();
    return out;
}

// ---------------------------------------------------------------------------
// Bitmap
// ---------------------------------------------------------------------------

Bitmap Bitmap::from_words(const std::vector<uint64_t> &raw, uint64_t num_bits) {
    Bitmap bm;
    const size_t num_words = words_for_bits(num_bits);
    const size_t num_chunks = (num_words + kChunkWords - 1) / kChunkWords;
    for (size_t c = 0; c < num_chunks; ++c) {
        const size_t begin = c * kChunkWords;
        const size_t end = std::min(begin + kChunkWords, num_words);
        std::vector<uint64_t> words(raw.begin() + static_cast<long>(begin),
                                    raw.begin() + static_cast<long>(end));
        Container container = Container::from_bitset(std::move(words));
        if (!container.empty())
            bm.chunks_.push_back({static_cast<uint16_t>(c), std::move(container)});
    }
    return bm;
}

std::vector<uint64_t> Bitmap::to_words(uint64_t num_bits) const {
    const size_t num_words = words_for_bits(num_bits);
    std::vector<uint64_t> raw(num_words, 0);
    for (const Chunk &chunk : chunks_) {
        const std::vector<uint64_t> bits = chunk.container.to_bitset();
        const size_t base = static_cast<size_t>(chunk.key) * kChunkWords;
        for (size_t i = 0; i < kChunkWords && base + i < num_words; ++i)
            raw[base + i] = bits[i];
    }
    mask_tail(raw, num_bits);
    return raw;
}

uint64_t Bitmap::cardinality() const {
    uint64_t c = 0;
    for (const Chunk &chunk : chunks_)
        c += chunk.container.cardinality();
    return c;
}

size_t Bitmap::serialized_bytes() const {
    const bool has_runs = std::any_of(chunks_.begin(), chunks_.end(), [](const Chunk &c) {
        return c.container.kind == Container::Kind::Run;
    });
    size_t bytes = has_runs ? 4 + (chunks_.size() + 7) / 8 : 8;
    bytes += 4 * chunks_.size();
    if (!has_runs || chunks_.size() >= 4)
        bytes += 4 * chunks_.size();
    for (const Chunk &chunk : chunks_)
        bytes += chunk.container.serialized_bytes();
    return bytes;
}

Bytes Bitmap::serialize() const {
    const size_t count = chunks_.size();
    const bool has_runs = std::any_of(chunks_.begin(), chunks_.end(), [](const Chunk &c) {
        return c.container.kind == Container::Kind::Run;
    });
    Bytes out;
    if (has_runs) {
        put_le(out, 12347U | (uint32_t(count - 1) << 16), 4);
        Bytes run_flags((count + 7) / 8, 0);
        for (size_t i = 0; i < count; ++i)
            if (chunks_[i].container.kind == Container::Kind::Run)
                run_flags[i / 8] |= uint8_t(1U << (i % 8));
        out.insert(out.end(), run_flags.begin(), run_flags.end());
    } else {
        put_le(out, 12346, 4);
        put_le(out, count, 4);
    }
    for (const auto &c : chunks_) {
        put_le(out, c.key, 2);
        put_le(out, c.container.cardinality() - 1, 2);
    }
    if (!has_runs || count >= 4) {
        size_t offset = out.size() + 4 * count;
        for (const auto &c : chunks_) {
            put_le(out, offset, 4);
            offset += c.container.serialized_bytes();
        }
    }
    for (const auto &c : chunks_) {
        switch (c.container.kind) {
        case Container::Kind::Array:
            for (auto v : c.container.values)
                put_le(out, v, 2);
            break;
        case Container::Kind::Bitset:
            for (auto w : c.container.bits)
                put_le(out, w, 8);
            break;
        case Container::Kind::Run:
            put_le(out, c.container.runs.size(), 2);
            for (auto r : c.container.runs) {
                put_le(out, r.start, 2);
                put_le(out, uint32_t(r.last) - r.start, 2);
            }
            break;
        }
    }
    return out;
}
Bitmap Bitmap::deserialize(const Bytes &payload) {
    ByteReader in(payload);
    const uint32_t cookie = static_cast<uint32_t>(in.get(4));
    bool has_runs = (cookie & 0xffffU) == 12347;
    if (!has_runs && cookie != 12346)
        throw std::runtime_error("invalid Roaring portable cookie");
    const uint64_t count = has_runs ? (cookie >> 16) + 1 : in.get(4);
    if (count > 65536)
        throw std::runtime_error("invalid Roaring container count");
    const Bytes flags = has_runs ? in.take((count + 7) / 8) : Bytes{};
    if (has_runs && count % 8 && (flags.back() >> (count % 8)))
        throw std::runtime_error("nonzero Roaring run flag padding");
    if (count > in.remaining() / 4)
        throw std::runtime_error("truncated Roaring descriptors");
    std::vector<uint16_t> keys;
    std::vector<uint32_t> cards;
    for (uint64_t i = 0; i < count; ++i) {
        const auto key = static_cast<uint16_t>(in.get(2));
        if (!keys.empty() && key <= keys.back())
            throw std::runtime_error("unsorted Roaring keys");
        keys.push_back(key);
        cards.push_back(static_cast<uint32_t>(in.get(2)) + 1);
    }
    std::vector<uint32_t> offsets;
    if (!has_runs || count >= 4)
        for (uint64_t i = 0; i < count; ++i)
            offsets.push_back(static_cast<uint32_t>(in.get(4)));
    Bitmap result;
    for (size_t i = 0; i < count; ++i) {
        if (!offsets.empty() && offsets[i] != in.position())
            throw std::runtime_error("invalid Roaring container offset");
        Container c;
        if (has_runs && (flags[i / 8] & (1U << (i % 8)))) {
            c.kind = Container::Kind::Run;
            const uint64_t nruns = in.get(2);
            if (!nruns || nruns > in.remaining() / 4)
                throw std::runtime_error("invalid Roaring run count");
            for (uint64_t r = 0; r < nruns; ++r) {
                const uint32_t start = static_cast<uint32_t>(in.get(2));
                const uint32_t last = start + static_cast<uint32_t>(in.get(2));
                if (last > 65535 || (!c.runs.empty() && start <= c.runs.back().last))
                    throw std::runtime_error("invalid Roaring run interval");
                c.runs.push_back({static_cast<uint16_t>(start), static_cast<uint16_t>(last)});
            }
        } else if (cards[i] <= 4096) {
            c.kind = Container::Kind::Array;
            for (uint32_t j = 0; j < cards[i]; ++j) {
                const uint16_t value = static_cast<uint16_t>(in.get(2));
                if (!c.values.empty() && value <= c.values.back())
                    throw std::runtime_error("unsorted Roaring array");
                c.values.push_back(value);
            }
        } else {
            c.kind = Container::Kind::Bitset;
            for (size_t j = 0; j < kChunkWords; ++j)
                c.bits.push_back(in.get(8));
        }
        if (c.cardinality() != cards[i])
            throw std::runtime_error("Roaring cardinality mismatch");
        result.chunks_.push_back({keys[i], std::move(c)});
    }
    in.require_end();
    return result;
}

// Only keys present in both operands can contribute, so an intersection walks
// the shorter chunk list; this is why AND over sparse data is so cheap here.
Bitmap bitmap_and(const Bitmap &a, const Bitmap &b) {
    Bitmap out;
    size_t i = 0, j = 0;
    while (i < a.chunks_.size() && j < b.chunks_.size()) {
        const uint16_t ka = a.chunks_[i].key, kb = b.chunks_[j].key;
        if (ka < kb)
            ++i;
        else if (kb < ka)
            ++j;
        else {
            Container c = container_and(a.chunks_[i].container, b.chunks_[j].container);
            if (!c.empty())
                out.chunks_.push_back({ka, std::move(c)});
            ++i;
            ++j;
        }
    }
    return out;
}

Bitmap bitmap_or(const Bitmap &a, const Bitmap &b) {
    Bitmap out;
    size_t i = 0, j = 0;
    while (i < a.chunks_.size() || j < b.chunks_.size()) {
        if (j >= b.chunks_.size() ||
            (i < a.chunks_.size() && a.chunks_[i].key < b.chunks_[j].key)) {
            out.chunks_.push_back(a.chunks_[i++]);
        } else if (i >= a.chunks_.size() || b.chunks_[j].key < a.chunks_[i].key) {
            out.chunks_.push_back(b.chunks_[j++]);
        } else {
            Container c = container_or(a.chunks_[i].container, b.chunks_[j].container);
            out.chunks_.push_back({a.chunks_[i].key, std::move(c)});
            ++i;
            ++j;
        }
    }
    return out;
}

} // namespace roaring
} // namespace bench
