#include "backends/codec_internal.hpp"
#include <deque>

namespace bench::detail {
namespace {
class SbhWriter final : public Writer {
  public:
    SbhWriter(uint64_t n, unsigned groups) : e_{Codec::Sbh, n, 7, groups, {}} {
        require(groups >= 1 && groups <= 4095, "SBH groups must be in 1..4095");
        put_le(e_.bytes, groups, 4);
    }
    void append(uint64_t v, uint64_t count) override {
        while (count) {
            const uint64_t take = std::min(count, uint64_t(e_.sbh_groups - position_));
            if (uniform(v, 7)) {
                if (pending_.count && pending_.value != v)
                    flush();
                pending_.value = v;
                pending_.count += take;
            } else {
                flush();
                for (uint64_t i = 0; i < take; ++i)
                    e_.bytes.push_back(static_cast<uint8_t>(v));
            }
            position_ += static_cast<unsigned>(take);
            count -= take;
            if (position_ == e_.sbh_groups) {
                flush();
                position_ = 0;
            }
        }
    }
    Encoding finish() override {
        flush();
        return std::move(e_);
    }

  private:
    void flush() {
        uint64_t n = pending_.count;
        while (n) {
            e_.bytes.push_back(static_cast<uint8_t>((pending_.value ? 0xc0 : 0x80) | (n & 63)));
            n >>= 6;
        }
        pending_ = {};
    }
    Encoding e_;
    Span pending_;
    unsigned position_ = 0;
};
class SbhCursor final : public Cursor {
  public:
    explicit SbhCursor(const Encoding &e) : e_(e), in_(e.bytes) {
        const uint64_t groups = in_.get(4);
        require(groups >= 1 && groups <= 4095 && groups == e.sbh_groups, "invalid SBH region size");
    }
    bool next(Span &s) override {
        if (!in_.remaining())
            return false;
        const uint64_t b = in_.get(1);
        if (!(b & 128)) {
            ++stats.literal_units;
            s = {b, 1};
        } else {
            ++stats.fill_units;
            ++stats.control_units;
            const uint64_t tag = b >> 6;
            uint64_t count = b & 63;
            // Equal prefixes continue one base-64 count, except at a known
            // super-bucket boundary. There are at most 4095 groups per region.
            const uint64_t left = e_.sbh_groups - position_;
            if (count != left && in_.remaining() && (e_.bytes[in_.position()] >> 6) == tag) {
                const uint64_t high = in_.get(1) & 63;
                ++stats.control_units;
                require(high != 0, "noncanonical SBH counter continuation");
                count += high << 6;
            }
            require(count && count <= left, "invalid SBH fill length");
            s = {tag == 3 ? 127ULL : 0ULL, count};
        }
        position_ += static_cast<unsigned>(s.count);
        if (position_ == e_.sbh_groups)
            position_ = 0;
        return true;
    }

  private:
    const Encoding &e_;
    ByteReader in_;
    unsigned position_ = 0;
};

// BBC uses the five control families in EP0650264A1, including long fills,
// one-bit exceptions, the opposite-byte special case and stream terminator.
// A logical length in the outer archive permits implicit trailing zeros.
class BbcWriter final : public Writer {
  public:
    explicit BbcWriter(uint64_t n) : e_{Codec::Bbc, n, 8, 128, {}} {}
    void append(uint64_t v, uint64_t count) override {
        if (!count)
            return;
        if (uniform(v, 8)) {
            if (!tail_.empty())
                flush_tail();
            const bool bit = v != 0;
            if (fill_count_ && bit != fill_bit_) {
                // n=0 encodes the counted fill followed by one opposite byte.
                emit_fill_tail(0);
                reset();
                --count;
            }
            if (count) {
                fill_bit_ = bit;
                fill_count_ += count;
            }
        } else {
            while (count--) {
                // With no preceding gap either exception polarity is legal.
                // This also compresses an isolated seven-one byte in one atom.
                const bool exception_bit = fill_count_ ? fill_bit_ : popcount64(v) == 7;
                const uint8_t diff = static_cast<uint8_t>(v ^ (exception_bit ? 255 : 0));
                if (tail_.empty() && popcount64(diff) == 1) {
                    fill_bit_ = exception_bit;
                    emit_exception(static_cast<unsigned>(ctz64(diff)));
                    reset();
                } else {
                    tail_.push_back(static_cast<uint8_t>(v));
                    if (tail_.size() == 15)
                        flush_tail();
                }
            }
        }
    }
    Encoding finish() override {
        if (!tail_.empty())
            flush_tail();
        // No code is needed for terminal zero bytes. A terminal one fill uses
        // the opposite-byte special case, which adds one zero padding byte.
        if (fill_count_ && fill_bit_)
            emit_fill_tail(0);
        e_.bytes.push_back(0);
        return std::move(e_);
    }

  private:
    void reset() {
        fill_count_ = 0;
        fill_bit_ = false;
        tail_.clear();
    }
    void long_count() {
        // Gap length is in bits, hence its low three bits are available for
        // the number of subsequent length bytes (0..7).
        const uint64_t bits = fill_count_ * 8;
        unsigned bytes = 1;
        while (bytes < 8 && (bits >> (bytes * 8)))
            ++bytes;
        put_le(e_.bytes, bits | (bytes - 1), bytes);
    }
    void emit_fill_tail(unsigned tail_size) {
        if (fill_count_ <= 3)
            e_.bytes.push_back(
                static_cast<uint8_t>((fill_count_ << 5) | (fill_bit_ ? 16 : 0) | tail_size));
        else {
            e_.bytes.push_back(static_cast<uint8_t>(0x80 | (fill_bit_ ? 16 : 0) | tail_size));
            long_count();
        }
    }
    void emit_exception(unsigned p) {
        if (fill_count_ <= 3)
            e_.bytes.push_back(
                static_cast<uint8_t>((fill_bit_ ? 0xe0 : 0xa0) | (fill_count_ << 3) | p));
        else {
            e_.bytes.push_back(static_cast<uint8_t>(0xc0 | (fill_bit_ ? 8 : 0) | p));
            long_count();
        }
    }
    void flush_tail() {
        emit_fill_tail(static_cast<unsigned>(tail_.size()));
        e_.bytes.insert(e_.bytes.end(), tail_.begin(), tail_.end());
        reset();
    }
    Encoding e_;
    bool fill_bit_ = false;
    uint64_t fill_count_ = 0;
    Bytes tail_;
};

class BbcCursor final : public Cursor {
  public:
    explicit BbcCursor(const Encoding &e) : in_(e.bytes), target_(group_count(e.num_bits, 8)) {}
    bool next(Span &s) override {
        for (;;) {
            Span candidate;
            if (fill_.count) {
                candidate = fill_;
                fill_ = {};
            } else if (tail_) {
                candidate = {in_.get(1), 1};
                --tail_;
            } else if (have_exception_) {
                candidate = {exception_, 1};
                have_exception_ = false;
            } else if (ended_) {
                if (produced_ < target_) {
                    s = {0, target_ - produced_};
                    produced_ = target_;
                    return true;
                }
                return false;
            } else {
                parse_atom();
                continue;
            }
            if (produced_ == target_) {
                require(candidate.value == 0 && candidate.count <= 1 && !extra_padding_,
                        "BBC data beyond logical length");
                extra_padding_ = true;
                continue;
            }
            require(candidate.count <= target_ - produced_, "BBC fill exceeds logical length");
            produced_ += candidate.count;
            s = candidate;
            return true;
        }
    }

  private:
    uint64_t get_long_count() {
        const uint64_t first = in_.get(1);
        const unsigned extra = static_cast<unsigned>(first & 7);
        uint64_t bits = first & 0xf8;
        for (unsigned i = 0; i < extra; ++i)
            bits |= in_.get(1) << (8 * (i + 1));
        require(bits >= 32 && (bits & 7) == 0, "invalid BBC extended fill length");
        return bits / 8;
    }
    void parse_atom() {
        require(in_.remaining() != 0, "missing BBC terminator");
        const unsigned header = static_cast<unsigned>(in_.get(1));
        ++stats.control_units;
        if (!header) {
            in_.require_end();
            ended_ = true;
            return;
        }
        const unsigned type = header >> 5;
        uint64_t count = 0;
        bool bit = false;
        if (type <= 4) {
            bit = (header & 16) != 0;
            count = type < 4 ? type : get_long_count();
            tail_ = header & 15;
            stats.literal_units += tail_;
            if (!tail_) {
                require(count > 0, "reserved BBC zero-fill/zero-tail control");
                have_exception_ = true;
                exception_ = bit ? 0 : 255;
            } else
                require(tail_ <= in_.remaining(), "truncated BBC literal tail");
        } else {
            ++stats.exception_units;
            unsigned p = header & 7;
            if (type == 5 || type == 7) {
                bit = type == 7;
                count = (header >> 3) & 3;
            } else {
                const unsigned field = (header >> 3) & 3;
                require(field <= 1, "reserved BBC extended exception control");
                bit = field != 0;
                count = get_long_count();
            }
            have_exception_ = true;
            exception_ = static_cast<uint8_t>((bit ? 255 : 0) ^ (1U << p));
        }
        if (count) {
            ++stats.fill_units;
            fill_ = {bit ? 255ULL : 0ULL, count};
        }
    }
    ByteReader in_;
    uint64_t target_, produced_ = 0;
    Span fill_;
    unsigned tail_ = 0;
    uint8_t exception_ = 0;
    bool ended_ = false, have_exception_ = false, extra_padding_ = false;
};
} // namespace
std::unique_ptr<Cursor> byte_cursor(const Encoding &e) {
    if (e.codec == Codec::Sbh)
        return std::make_unique<SbhCursor>(e);
    return std::make_unique<BbcCursor>(e);
}
std::unique_ptr<Writer> byte_writer(Codec c, uint64_t n, unsigned groups) {
    if (c == Codec::Sbh)
        return std::make_unique<SbhWriter>(n, groups);
    return std::make_unique<BbcWriter>(n);
}
} // namespace bench::detail
