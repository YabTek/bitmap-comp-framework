#include "backends/codec_internal.hpp"
#include <deque>

namespace bench::detail {
namespace {
class WordCursor final : public Cursor {
  public:
    explicit WordCursor(const Encoding &e) : e_(e), in_(e.bytes) {
        if (e.codec == Codec::Val) {
            const auto selector = in_.get(1);
            require((selector & 15) == 1 && ((selector >> 4) == e.width / 15),
                    "invalid VAL selector");
        }
    }
    bool next(Span &s) override {
        if (!pending_.empty()) {
            s = pending_.front();
            pending_.pop_front();
            return true;
        }
        if (e_.codec == Codec::Ewah)
            return ewah(s);
        if (e_.codec == Codec::Val)
            return val(s);
        if (!in_.remaining())
            return false;
        const uint32_t word = static_cast<uint32_t>(in_.get(4));
        const bool is_literal = e_.codec == Codec::Concise ? (word >> 31) != 0 : (word >> 31) == 0;
        if (is_literal) {
            ++stats.literal_units;
            s = {word & 0x7fffffffU, 1};
            return true;
        }
        const uint32_t p = (word >> 25) & 31;
        const uint64_t c = word & 0x1ffffffU;
        const uint64_t base = ((word >> 30) & 1) ? 0x7fffffffULL : 0;
        ++stats.fill_units;
        if (p)
            ++stats.exception_units;
        if (e_.codec == Codec::Plwah) {
            require(c != 0, "zero PLWAH fill counter");
            s = {base, c};
            if (p)
                pending_.push_back({base ^ (uint64_t(1) << (p - 1)), 1});
        } else {
            if (p) {
                s = {base ^ (uint64_t(1) << (p - 1)), 1};
                if (c)
                    pending_.push_back({base, c});
            } else
                s = {base, c + 1};
        }
        return true;
    }

  private:
    bool ewah(Span &s) {
        while (!literals_) {
            if (!in_.remaining())
                return false;
            const uint64_t marker = in_.get(8);
            ++stats.control_units;
            const uint64_t count = (marker >> 1) & 0xffffffffULL;
            literals_ = marker >> 33;
            require(literals_ <= in_.remaining() / 8, "truncated EWAH literal sequence");
            if (count) {
                ++stats.fill_units;
                s = {marker & 1 ? ~uint64_t(0) : 0, count};
                return true;
            }
            if (!literals_) {
                require(e_.num_bits == 0 && in_.remaining() == 0, "empty EWAH marker");
                return false;
            }
        }
        s = {in_.get(8), 1};
        ++stats.literal_units;
        --literals_;
        return true;
    }
    bool val(Span &s) {
        const uint64_t target = group_count(e_.num_bits, e_.width);
        if (val_groups_ == target) {
            if (slot_ < slots_) {
                const unsigned bits_left = (slots_ - slot_) * e_.width;
                require((word_ & low_mask(bits_left)) == 0, "nonzero unused VAL payload slots");
                const uint64_t flags = word_ >> 60;
                const uint64_t unused = low_mask(4 - slot_);
                require((flags & unused) == 0, "nonzero unused VAL flags");
            }
            require(in_.remaining() == 0, "extra VAL words");
            return false;
        }
        if (slot_ == slots_) {
            require(in_.remaining() >= 8, "truncated VAL word");
            word_ = in_.get(8);
            ++stats.control_units;
            slots_ = 60 / e_.width;
            slot_ = 0;
            require(((word_ >> 60) & low_mask(4 - slots_)) == 0, "nonzero reserved VAL flags");
        }
        const unsigned shift = (slots_ - 1 - slot_) * e_.width;
        const uint64_t payload = (word_ >> shift) & low_mask(e_.width);
        const bool fill = ((word_ >> (63 - slot_)) & 1) != 0;
        ++slot_;
        if (fill) {
            ++stats.fill_units;
            const uint64_t count = payload & low_mask(e_.width - 1);
            require(count && count <= target - val_groups_, "invalid VAL fill counter");
            s = {payload >> (e_.width - 1) ? low_mask(e_.width) : 0, count};
        } else {
            ++stats.literal_units;
            s = {payload, 1};
        }
        val_groups_ += s.count;
        return true;
    }
    const Encoding &e_;
    ByteReader in_;
    std::deque<Span> pending_;
    uint64_t literals_ = 0, word_ = 0, val_groups_ = 0;
    unsigned slot_ = 0, slots_ = 0;
};

class WordWriter final : public Writer {
  public:
    WordWriter(Codec codec, uint64_t n, unsigned width) : e_{codec, n, width, 128, {}} {
        if (codec == Codec::Val)
            e_.bytes.push_back(static_cast<uint8_t>((width / 15) << 4 | 1));
    }
    void append(uint64_t v, uint64_t count) override {
        if (!count)
            return;
        v &= low_mask(e_.width);
        if (e_.codec == Codec::Ewah) {
            ewah(v, count);
            return;
        }
        if (e_.codec == Codec::Val) {
            val(v, count);
            return;
        }
        const bool is_uniform = uniform(v, 31);
        if (e_.codec == Codec::Plwah) {
            if (is_uniform) {
                const uint32_t base = 0x80000000U | (v ? 0x40000000U : 0);
                while (count) {
                    if (have_word_ && (last_ & 0xfe000000U) == base) {
                        const uint64_t add =
                            std::min(count, uint64_t(0x1ffffffU - (last_ & 0x1ffffffU)));
                        if (add) {
                            last_ += static_cast<uint32_t>(add);
                            count -= add;
                            continue;
                        }
                    }
                    const uint64_t add = std::min(count, uint64_t(0x1ffffffU));
                    push32(base | static_cast<uint32_t>(add));
                    count -= add;
                }
            } else {
                while (count--) {
                    bool attached = false;
                    if (have_word_ && (last_ >> 31) && ((last_ >> 25) & 31) == 0) {
                        const uint64_t base = last_ & 0x40000000U ? 0x7fffffffULL : 0;
                        const uint64_t diff = v ^ base;
                        if (popcount64(diff) == 1) {
                            last_ |= static_cast<uint32_t>(ctz64(diff) + 1) << 25;
                            attached = true;
                        }
                    }
                    if (!attached)
                        push32(static_cast<uint32_t>(v));
                }
            }
        } else {
            if (is_uniform)
                concise_fill(v != 0, count, 0);
            else {
                const uint64_t diff0 = v, diff1 = v ^ 0x7fffffffULL;
                for (uint64_t i = 0; i < count; ++i) {
                    if (popcount64(diff0) == 1)
                        concise_fill(false, 1, ctz64(diff0) + 1);
                    else if (popcount64(diff1) == 1)
                        concise_fill(true, 1, ctz64(diff1) + 1);
                    else
                        push32(0x80000000U | static_cast<uint32_t>(v));
                }
            }
        }
    }
    Encoding finish() override {
        if (e_.codec == Codec::Val) {
            flush_val_pending();
            flush_val();
        } else if (e_.codec == Codec::Ewah) {
            if (!have_marker_)
                start_marker();
            update_marker();
        } else if (have_word_)
            put_le(e_.bytes, last_, 4);
        return std::move(e_);
    }

  private:
    void push32(uint32_t word) {
        if (have_word_)
            put_le(e_.bytes, last_, 4);
        last_ = word;
        have_word_ = true;
    }
    void concise_fill(bool bit, uint64_t count, unsigned p) {
        while (count) {
            // A pure following fill may extend a previous fill whose exception
            // is already confined to its first group.
            if (!p && have_word_ && !(last_ >> 31) && bool(last_ & 0x40000000U) == bit) {
                const uint64_t room = 0x1ffffffU - (last_ & 0x1ffffffU);
                const uint64_t add = std::min(count, room);
                if (add) {
                    last_ += static_cast<uint32_t>(add);
                    count -= add;
                    continue;
                }
            }
            const uint64_t take = std::min(count, uint64_t(1) << 25);
            push32((bit ? 0x40000000U : 0) | (static_cast<uint32_t>(p) << 25) |
                   static_cast<uint32_t>(take - 1));
            count -= take;
            p = 0;
        }
    }
    void start_marker() {
        if (have_marker_)
            update_marker();
        marker_pos_ = e_.bytes.size();
        put_le(e_.bytes, 0, 8);
        have_marker_ = true;
        fill_bit_ = false;
        fill_count_ = literal_count_ = 0;
    }
    void update_marker() {
        set_le(e_.bytes, marker_pos_,
               uint64_t(fill_bit_) | (fill_count_ << 1) | (literal_count_ << 33), 8);
    }
    void ewah(uint64_t v, uint64_t count) {
        const bool is_uniform = uniform(v, 64);
        if (is_uniform) {
            while (count) {
                if (!have_marker_ || literal_count_ || (fill_count_ && fill_bit_ != (v != 0)) ||
                    fill_count_ == 0xffffffffULL)
                    start_marker();
                fill_bit_ = v != 0;
                const uint64_t take = std::min<uint64_t>(count, 0xffffffffULL - fill_count_);
                fill_count_ += take;
                count -= take;
            }
        } else {
            while (count--) {
                if (!have_marker_ || literal_count_ == 0x7fffffffULL)
                    start_marker();
                put_le(e_.bytes, v, 8);
                ++literal_count_;
            }
        }
    }
    void flush_val() {
        if (val_used_) {
            put_le(e_.bytes, val_word_, 8);
            val_word_ = 0;
            val_used_ = 0;
        }
    }
    void val_token(uint64_t payload, bool fill) {
        const unsigned slots = 60 / e_.width;
        if (val_used_ == slots)
            flush_val();
        if (fill)
            val_word_ |= uint64_t(1) << (63 - val_used_);
        val_word_ |= payload << ((slots - 1 - val_used_) * e_.width);
        ++val_used_;
    }
    void val(uint64_t v, uint64_t count) {
        // Hold one span so adjacent equal fills merge before physical packing.
        if (val_pending_.count && val_pending_.value == v && uniform(v, e_.width))
            val_pending_.count += count;
        else {
            flush_val_pending();
            val_pending_ = {v, count};
        }
    }
    void flush_val_pending() {
        if (!val_pending_.count)
            return;
        uint64_t n = val_pending_.count;
        if (uniform(val_pending_.value, e_.width)) {
            while (n) {
                const uint64_t take = std::min(n, low_mask(e_.width - 1));
                val_token((val_pending_.value ? uint64_t(1) << (e_.width - 1) : 0) | take, true);
                n -= take;
            }
        } else
            while (n--)
                val_token(val_pending_.value, false);
        val_pending_ = {};
    }
    Encoding e_;
    bool have_word_ = false, have_marker_ = false, fill_bit_ = false;
    uint32_t last_ = 0;
    size_t marker_pos_ = 0;
    uint64_t fill_count_ = 0, literal_count_ = 0, val_word_ = 0;
    unsigned val_used_ = 0;
    Span val_pending_;
};
} // namespace
std::unique_ptr<Cursor> word_cursor(const Encoding &e) { return std::make_unique<WordCursor>(e); }
std::unique_ptr<Writer> word_writer(Codec c, uint64_t n, unsigned w) {
    return std::make_unique<WordWriter>(c, n, w);
}
} // namespace bench::detail
