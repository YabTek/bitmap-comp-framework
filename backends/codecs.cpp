#include "backends/archive.hpp"
#include "backends/codec_internal.hpp"

namespace bench {
using namespace detail;
namespace {
std::unique_ptr<Cursor> cursor(const Encoding &e) {
    return e.codec == Codec::Sbh || e.codec == Codec::Bbc ? byte_cursor(e) : word_cursor(e);
}
std::unique_ptr<Writer> writer(Codec c, uint64_t n, unsigned width, unsigned groups) {
    return c == Codec::Sbh || c == Codec::Bbc ? byte_writer(c, n, groups)
                                              : word_writer(c, n, width);
}
unsigned width_for(Codec c) {
    switch (c) {
    case Codec::Ewah:
        return 64;
    case Codec::Concise:
    case Codec::Plwah:
        return 31;
    case Codec::Val:
        return 15;
    case Codec::Sbh:
        return 7;
    case Codec::Bbc:
        return 8;
    }
    throw std::runtime_error("unknown codec");
}
void validate(const Encoding &e) {
    auto r = cursor(e);
    const uint64_t groups = group_count(e.num_bits, e.width);
    uint64_t seen = 0;
    Span s;
    while (r->next(s)) {
        require(s.count && s.count <= groups - seen,
                "encoded group count does not match logical length");
        require((s.value & ~low_mask(e.width)) == 0, "literal exceeds group width");
        seen += s.count;
        if (seen == groups && e.num_bits % e.width)
            require((s.value >> (e.num_bits % e.width)) == 0,
                    "nonzero encoded padding outside logical length");
    }
    require(seen == groups, "encoded bitmap is shorter than logical length");
}
// Split only the current literal when VAL inputs have different segment sizes.
// Uniform spans remain run counters; the full input is never expanded.
class AlignedCursor {
  public:
    AlignedCursor(const Encoding &e, unsigned width)
        : input_(cursor(e)), native_(e.width), width_(width),
          left_(group_count(e.num_bits, width)) {
        require(native_ % width_ == 0, "incompatible segment lengths");
    }
    bool next(Span &out) {
        if (!left_)
            return false;
        if (piece_left_) {
            out = {piece_ & low_mask(width_), 1};
            piece_ >>= width_;
            --piece_left_;
        } else {
            Span s;
            require(input_->next(s), "input ended during logical operation");
            if (native_ == width_)
                out = s;
            else if (uniform(s.value, native_))
                out = {s.value ? low_mask(width_) : 0, s.count * (native_ / width_)};
            else {
                require(s.count == 1, "invalid repeated literal");
                piece_ = s.value;
                piece_left_ = native_ / width_;
                return next(out);
            }
        }
        out.count = std::min(out.count, left_);
        left_ -= out.count;
        return true;
    }

  private:
    std::unique_ptr<Cursor> input_;
    unsigned native_, width_, piece_left_ = 0;
    uint64_t left_, piece_ = 0;
};

Encoding sbh_combine(const Encoding &a, const Encoding &b, bool is_and) {
    auto ra = cursor(a), rb = cursor(b);
    const unsigned output_groups = std::min(a.sbh_groups, b.sbh_groups);
    auto out = byte_writer(Codec::Sbh, a.num_bits, output_groups);
    Span sa, sb;
    auto expand = [&](Cursor &r, Span &pending, uint64_t groups) {
        std::vector<uint64_t> words(words_for_bits(groups * 7), 0);
        uint64_t at = 0;
        while (at < groups) {
            if (!pending.count)
                require(r.next(pending), "truncated SBH operand");
            const uint64_t take = std::min(groups - at, pending.count);
            if (pending.value)
                for (uint64_t i = 0; i < take; ++i)
                    write_group(words, (at + i) * 7, 7, pending.value);
            at += take;
            pending.count -= take;
        }
        return words;
    };
    uint64_t left = group_count(a.num_bits, 7);
    while (left) {
        const uint64_t groups = std::min(left, uint64_t(output_groups));
        auto wa = expand(*ra, sa, groups), wb = expand(*rb, sb, groups);
        for (size_t i = 0; i < wa.size(); ++i)
            wa[i] = is_and ? wa[i] & wb[i] : wa[i] | wb[i];
        for (uint64_t i = 0; i < groups; ++i)
            out->append(read_group(wa, i * 7, 7), 1);
        left -= groups;
    }
    return out->finish();
}
} // namespace
const char *codec_name(Codec c) {
    switch (c) {
    case Codec::Ewah:
        return "ewah";
    case Codec::Concise:
        return "concise";
    case Codec::Plwah:
        return "plwah";
    case Codec::Val:
        return "val";
    case Codec::Sbh:
        return "sbh";
    case Codec::Bbc:
        return "bbc";
    }
    return "unknown";
}
Encoding encode_bitmap(Codec c, const std::vector<uint64_t> &raw, uint64_t n,
                       CodecOptions options) {
    require(n <= (uint64_t(1) << 32), "supported universe is 0..2^32 bits");
    require(raw.size() == words_for_bits(n), "bitmap word count mismatch");
    require(n % 64 == 0 || raw.empty() || (raw.back() >> (n % 64)) == 0, "nonzero input padding");
    unsigned width = width_for(c);
    if (c == Codec::Val) {
        if (options.val_segment_bits == 0) {
            Encoding best;
            for (unsigned w : {15U, 30U, 60U}) {
                options.val_segment_bits = w;
                auto e = encode_bitmap(c, raw, n, options);
                if (best.bytes.empty() || e.bytes.size() < best.bytes.size())
                    best = std::move(e);
            }
            return best;
        }
        width = options.val_segment_bits;
        require(width == 15 || width == 30 || width == 60,
                "VAL segment size must be 15, 30, 60 or auto");
    }
    auto out = writer(c, n, width, options.sbh_groups);
    const uint64_t groups = group_count(n, width);
    for (uint64_t i = 0; i < groups; ++i)
        out->append(read_group(raw, i * width, width), 1);
    return out->finish();
}
Encoding load_encoding(Codec c, uint64_t n, const Bytes &payload) {
    require(n <= (uint64_t(1) << 32), "logical length exceeds 2^32 bits");
    Encoding e{c, n, width_for(c), 128, payload};
    if (c == Codec::Val) {
        require(!payload.empty(), "missing VAL selector");
        e.width = 15 * (payload[0] >> 4);
        require(e.width == 15 || e.width == 30 || e.width == 60, "invalid VAL segment size");
    }
    if (c == Codec::Sbh) {
        ByteReader in(payload);
        e.sbh_groups = static_cast<unsigned>(in.get(4));
    }
    validate(e);
    return e;
}
std::vector<uint64_t> decode_bitmap(const Encoding &e) {
    auto r = cursor(e);
    std::vector<uint64_t> result(words_for_bits(e.num_bits), 0);
    uint64_t start = 0;
    Span s;
    while (r->next(s)) {
        if (s.value)
            for (uint64_t i = 0; i < s.count; ++i)
                write_group(result, (start + i) * e.width, e.width, s.value);
        start += s.count;
    }
    mask_tail(result, e.num_bits);
    return result;
}
uint64_t encoding_cardinality(const Encoding &e) {
    auto r = cursor(e);
    Span s;
    uint64_t result = 0;
    while (r->next(s))
        result += uint64_t(popcount64(s.value)) * s.count;
    return result;
}
RepresentationStats encoding_statistics(const Encoding &e) {
    auto r = cursor(e);
    Span s;
    while (r->next(s)) {
    }
    return r->stats;
}
Encoding combine_bitmaps(const Encoding &a, const Encoding &b, bool is_and) {
    require(a.codec == b.codec && a.num_bits == b.num_bits, "incompatible logical operands");
    if (a.codec == Codec::Sbh)
        return sbh_combine(a, b, is_and);
    const unsigned w = std::min(a.width, b.width);
    AlignedCursor ra(a, w), rb(b, w);
    auto out = writer(a.codec, a.num_bits, w, a.sbh_groups);
    Span sa, sb;
    while (true) {
        if (!sa.count && !ra.next(sa))
            break;
        if (!sb.count)
            require(rb.next(sb), "incompatible operand lengths");
        const uint64_t take = std::min(sa.count, sb.count);
        out->append(is_and ? sa.value & sb.value : sa.value | sb.value, take);
        sa.count -= take;
        sb.count -= take;
    }
    return out->finish();
}
std::vector<Span> inspect_spans(const Encoding &e) {
    auto r = cursor(e);
    std::vector<Span> result;
    Span s;
    while (r->next(s))
        result.push_back(s);
    return result;
}
} // namespace bench
