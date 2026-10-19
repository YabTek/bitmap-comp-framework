#include "backends/archive.hpp"
#include "backends/codecs.hpp"
#include "backends/factory.hpp"
#include "backends/roaring_bitmap.hpp"
#include "common/bits.hpp"
#include <algorithm>
#include <functional>
#include <iostream>
#include <random>
#include <stdexcept>

using namespace bench;
namespace {
uint64_t checks = 0;
void check(bool condition, const std::string &context) {
    ++checks;
    if (!condition)
        throw std::runtime_error(context);
}
void rejects(const std::function<void()> &f, const std::string &context) {
    bool failed = false;
    try {
        f();
    } catch (const std::exception &) {
        failed = true;
    }
    check(failed, "expected rejection: " + context);
}
std::vector<uint64_t> raw(uint64_t n, const std::function<bool(uint64_t)> &present) {
    std::vector<uint64_t> r(words_for_bits(n), 0);
    for (uint64_t i = 0; i < n; ++i)
        if (present(i))
            set_bit(r, i);
    return r;
}
std::vector<uint64_t> combine(const std::vector<std::vector<uint64_t>> &data,
                              const std::vector<size_t> &ids, bool is_and) {
    auto result = data.at(ids.at(0));
    for (size_t k = 1; k < ids.size(); ++k)
        for (size_t i = 0; i < result.size(); ++i)
            result[i] = is_and ? result[i] & data.at(ids[k])[i] : result[i] | data.at(ids[k])[i];
    return result;
}
void check_dataset(uint64_t n, const std::vector<std::vector<uint64_t>> &data,
                   CodecOptions options = {}) {
    for (const auto &name : backend_names()) {
        auto backend = make_backend(name, options);
        backend->build(data, n);
        const Bytes bytes = backend->serialize();
        check(bytes.size() == backend->serialized_bytes(), name + " saved size");
        auto loaded = make_backend(name, options);
        loaded->deserialize(bytes);
        check(loaded->serialize() == bytes, name + " stable archive round trip");
        for (size_t a = 0; a < data.size(); ++a) {
            check(loaded->query_or({a})->to_words() == data[a],
                  name + " decode N=" + std::to_string(n));
            for (size_t b = 0; b < data.size(); ++b) {
                const std::vector<size_t> ids{a, b};
                for (bool is_and : {false, true}) {
                    const auto expected = combine(data, ids, is_and);
                    auto r = is_and ? loaded->query_and(ids) : loaded->query_or(ids);
                    const std::string context =
                        name + (is_and ? " AND" : " OR") + " N=" + std::to_string(n) +
                        " pair=" + std::to_string(a) + "," + std::to_string(b);
                    check(r->to_words() == expected, context);
                    check(r->cardinality() == cardinality(expected), context + " cardinality");
                }
            }
        }
        const std::vector<size_t> repeated{0, data.size() - 1, 0};
        check(loaded->query_and(repeated)->to_words() == combine(data, repeated, true),
              name + " multi AND");
        check(loaded->query_or(repeated)->to_words() == combine(data, repeated, false),
              name + " multi OR");
        rejects([&] { (void)loaded->query_and({}); }, name + " empty query");
        rejects([&] { (void)loaded->query_or({data.size()}); }, name + " invalid query index");
    }
}
void boundaries() {
    std::mt19937_64 rng(20261020);
    const std::vector<uint64_t> lengths{
        0,  1,  6,   7,   8,   14,  15,  16,  30,  31,  32,  60,   61,    62,    63,    64,
        65, 93, 105, 124, 127, 128, 255, 448, 449, 896, 897, 4096, 65535, 65536, 65537, 131073};
    for (uint64_t n : lengths) {
        std::vector<std::vector<uint64_t>> data;
        data.push_back(raw(n, [](uint64_t) { return false; }));
        data.push_back(raw(n, [](uint64_t) { return true; }));
        data.push_back(raw(n, [](uint64_t i) { return i % 2 == 0; }));
        data.push_back(
            raw(n, [&](uint64_t i) { return i == 0 || i + 1 == n || i == 65535 || i == 65536; }));
        data.push_back(raw(n, [](uint64_t i) { return (i / 97) % 3 == 1; }));
        data.push_back(raw(n, [&](uint64_t) { return rng() % 101 < 7; }));
        data.push_back(raw(n, [&](uint64_t) { return rng() % 101 < 94; }));
        check_dataset(n, data);
    }
    // VAL's smallest fill counter must split a run across several segments.
    check_dataset(262145, {raw(262145, [](uint64_t) { return false; }),
                           raw(262145, [](uint64_t) { return true; })});
}
void settings_and_random() {
    std::mt19937_64 rng(873);
    for (unsigned width : {0U, 15U, 30U, 60U})
        for (unsigned groups : {1U, 8U, 63U, 64U, 70U, 128U, 4095U}) {
            const uint64_t n = 1009;
            auto a = raw(n, [&](uint64_t i) { return i % 71 < 11; });
            auto b = raw(n, [&](uint64_t) { return rng() % 19 < 9; });
            check_dataset(n, {a, b}, {width, groups});
        }
    for (unsigned trial = 0; trial < 100; ++trial) {
        const uint64_t n = rng() % 5000;
        const auto a = raw(n, [&](uint64_t) { return rng() % 100 < rng() % 100; });
        const auto b = raw(n, [&](uint64_t) { return rng() % 100 < rng() % 100; });
        check_dataset(n, {a, b}, {15, 128});
    }
    // Same logical operands stored at different VAL lengths.
    for (unsigned aw : {15U, 30U, 60U})
        for (unsigned bw : {15U, 30U, 60U}) {
            for (uint64_t n : {1ULL, 31ULL, 105ULL, 1001ULL}) {
                const auto a = raw(n, [](uint64_t i) { return i % 17 < 5; });
                const auto b = raw(n, [](uint64_t i) { return i % 29 < 11; });
                const auto ea = encode_bitmap(Codec::Val, a, n, {aw, 128});
                const auto eb = encode_bitmap(Codec::Val, b, n, {bw, 128});
                for (bool op : {false, true})
                    check(decode_bitmap(combine_bitmaps(ea, eb, op)) == combine({a, b}, {0, 1}, op),
                          "mixed VAL lengths");
            }
        }
}
void mixed_sbh_regions() {
    const uint64_t n = 1001;
    auto a = raw(n, [](uint64_t i) { return i % 43 < 7; });
    auto b = raw(n, [](uint64_t i) { return i % 71 < 29; });
    for (unsigned ag : {1U, 8U, 63U, 64U, 128U, 4095U})
        for (unsigned bg : {1U, 8U, 63U, 64U, 128U, 4095U}) {
            const auto ea = encode_bitmap(Codec::Sbh, a, n, {15, ag});
            const auto eb = encode_bitmap(Codec::Sbh, b, n, {15, bg});
            for (bool op : {false, true}) {
                auto result = combine_bitmaps(ea, eb, op);
                check(decode_bitmap(result) == combine({a, b}, {0, 1}, op), "mixed SBH regions");
                check(result.sbh_groups == std::min(ag, bg), "SBH output region bound");
            }
        }
}

uint64_t le_word(const Bytes &b, unsigned size, size_t offset = 0) {
    uint64_t word = 0;
    for (unsigned i = 0; i < size; ++i)
        word |= uint64_t(b.at(offset + i)) << (8 * i);
    return word;
}
void golden_examples() {
    for (unsigned p = 0; p < 8; ++p)
        for (bool ones : {false, true}) {
            const uint64_t value = (ones ? 255U : 0U) ^ (1U << p);
            const auto e = encode_bitmap(Codec::Bbc, {value}, 8);
            check(e.bytes == Bytes({static_cast<uint8_t>((ones ? 0xe0 : 0xa0) | p), 0}),
                  "BBC standalone exception polarity");
        }
    const auto pl = encode_bitmap(Codec::Plwah, raw(124, [](uint64_t i) { return i == 93; }), 124);
    check(pl.bytes.size() == 4 && le_word(pl.bytes, 4) == 0x82000003U,
          "PLWAH fields and uniform count");
    const auto co = encode_bitmap(Codec::Concise, raw(124, [](uint64_t i) { return i == 0; }), 124);
    check(co.bytes.size() == 4 && le_word(co.bytes, 4) == 0x02000003U,
          "CONCISE fields and count-minus-one");
    const auto e = encode_bitmap(Codec::Ewah, {0, 0, 0x55, 0x33}, 256);
    check(e.bytes.size() == 24 && le_word(e.bytes, 8) == ((uint64_t(2) << 33) | 4),
          "EWAH marker: K=2 M=2");
    const std::vector<uint64_t> sbraw = raw(56, [](uint64_t i) {
        const uint64_t groups[]{0, 0, 0, 0x55, 0x7f, 0x7f, 0x33, 0};
        return ((groups[i / 7] >> (i % 7)) & 1) != 0;
    });
    const auto sb = encode_bitmap(Codec::Sbh, sbraw, 56, {15, 8});
    check(sb.bytes == Bytes({8, 0, 0, 0, 0x83, 0x55, 0xc2, 0x33, 0x81}), "SBH example bytes");
    const auto sb70 = encode_bitmap(Codec::Sbh, std::vector<uint64_t>(words_for_bits(490), 0), 490);
    check(sb70.bytes == Bytes({128, 0, 0, 0, 0x86, 0x81}), "SBH continuation for 70 groups");
    const auto bbc = load_encoding(Codec::Bbc, 32, {0x42, 0xa5, 0x5a, 0});
    check(decode_bitmap(bbc) == std::vector<uint64_t>{0x5aa50000},
          "BBC short fill plus two literals");
    const auto bbc2 = encode_bitmap(Codec::Bbc, {0x5aa50000}, 32);
    check(bbc2.bytes == Bytes({0x42, 0xa5, 0x5a, 0}), "BBC exact handout control");
    const std::vector<std::pair<Bytes, std::vector<uint8_t>>> atoms{
        {{0x02, 0x55, 0xaa, 0}, {0x55, 0xaa}},
        {{0x81, 0x20, 0x55, 0}, {0, 0, 0, 0, 0x55}},
        {{0xb3, 0}, {0, 0, 8}},
        {{0xf3, 0}, {255, 255, 247}},
        {{0xc3, 0x20, 0}, {0, 0, 0, 0, 8}},
        {{0xcb, 0x20, 0}, {255, 255, 255, 255, 247}},
        {{0x50, 0}, {255, 255, 0}},
        {{0x90, 0x20, 0}, {255, 255, 255, 255, 0}},
        {{0x80, 0x20, 0}, {0, 0, 0, 0, 255}},
    };
    for (const auto &pair : atoms) {
        Bytes expected = pair.second;
        const auto actual =
            decode_bitmap(load_encoding(Codec::Bbc, expected.size() * 8, pair.first));
        for (size_t i = 0; i < expected.size(); ++i)
            check(((actual[i / 8] >> (8 * (i % 8))) & 255) == expected[i],
                  "BBC control family decoding");
    }
    // Extended count uses a second length byte: 300 bytes = 2400 bits.
    Bytes long_bbc{0x81, 0x61, 0x09, 0x55, 0};
    const auto lr = decode_bitmap(load_encoding(Codec::Bbc, 301 * 8, long_bbc));
    check(lr[300 / 8] >> (8 * (300 % 8)) == 0x55, "BBC multi-byte length");
}
void exhaustive_bbc_bytes() {
    const auto mask = encode_bitmap(Codec::Bbc, {0xa55a}, 16);
    for (uint64_t value = 0; value < 65536; ++value) {
        const auto encoded = encode_bitmap(Codec::Bbc, {value}, 16);
        const auto loaded = load_encoding(Codec::Bbc, 16, encoded.bytes);
        check(decode_bitmap(loaded) == std::vector<uint64_t>{value},
              "BBC exhaustive two-byte round trip");
        check(encoding_cardinality(loaded) == uint64_t(popcount64(value)),
              "BBC exhaustive cardinality");
        check(decode_bitmap(combine_bitmaps(loaded, mask, true)) ==
                  std::vector<uint64_t>{value & 0xa55a},
              "BBC exhaustive AND");
        check(decode_bitmap(combine_bitmaps(loaded, mask, false)) ==
                  std::vector<uint64_t>{value | 0xa55a},
              "BBC exhaustive OR");
    }
}

void invalid_data() {
    for (const auto &name : backend_names()) {
        auto b = make_backend(name);
        rejects([&] { b->build({{uint64_t(2)}}, 1); }, name + " input padding");
        rejects([&] { b->build({{}}, 1); }, name + " short input");
        b->build({{1}}, 1);
        const Bytes good = b->serialize();
        for (size_t len = 0; len < good.size(); ++len) {
            const Bytes cut(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(len));
            rejects([&] { b->deserialize(cut); }, name + " truncated archive");
        }
        auto extra = good;
        extra.push_back(42);
        rejects([&] { b->deserialize(extra); }, name + " trailing archive bytes");
        check(b->query_or({0})->to_words() == std::vector<uint64_t>{1},
              name + " failed load preserves previous state");
    }
    rejects([] { (void)load_encoding(Codec::Bbc, 8, {0x81}); }, "BBC missing long count");
    rejects([] { (void)load_encoding(Codec::Bbc, 8, {0x55}); }, "BBC missing tail");
    rejects([] { (void)load_encoding(Codec::Bbc, 8, {0xa0}); }, "BBC missing terminator");
    rejects([] { (void)load_encoding(Codec::Sbh, 7, {8, 0, 0, 0, 0x80}); }, "SBH zero counter");
    rejects([] { (void)load_encoding(Codec::Plwah, 31, {0, 0, 0, 0x80}); }, "PLWAH zero counter");
    rejects([] { (void)load_encoding(Codec::Val, 15, {0x71}); }, "VAL illegal segment length");
    rejects([] { (void)load_encoding(Codec::Ewah, 64, {0, 0, 0, 0, 0, 0, 0, 0}); },
            "EWAH empty marker");
}
void large_counters() {
    // Reach 25-bit counters without allocating a billion logical bits.
    const uint64_t groups = (uint64_t(1) << 25) + 17;
    for (Codec codec : {Codec::Plwah, Codec::Concise})
        for (bool ones : {false, true}) {
            Bytes payload;
            if (codec == Codec::Plwah) {
                put_le(payload, 0x80000000U | (ones ? 0x40000000U : 0) | 0x1ffffffU, 4);
                put_le(payload, 0x80000000U | (ones ? 0x40000000U : 0) | 18U, 4);
            } else {
                put_le(payload, (ones ? 0x40000000U : 0) | 0x1ffffffU, 4);
                put_le(payload, (ones ? 0x40000000U : 0) | 16U, 4);
            }
            auto encoded = load_encoding(codec, groups * 31, payload);
            check(encoding_cardinality(encoded) == (ones ? groups * 31 : 0),
                  "large exception-format counter");
            auto result = combine_bitmaps(encoded, encoded, true);
            check(result.bytes.size() == 8, "large counter split during compressed query");
            check(encoding_cardinality(load_encoding(codec, groups * 31, result.bytes)) ==
                      (ones ? groups * 31 : 0),
                  "large counter result");
        }
    // Largest supported universe: a BBC ones fill plus one zero padding byte.
    Bytes payload{0x90, 0x04, 0, 0, 0, 1, 0};
    auto bbc = load_encoding(Codec::Bbc, uint64_t(1) << 32, payload);
    check(encoding_cardinality(bbc) == (uint64_t(1) << 32), "BBC 33-bit extended length");
    auto result = combine_bitmaps(bbc, bbc, false);
    check(encoding_cardinality(result) == (uint64_t(1) << 32), "BBC maximum-universe operation");
}
void mutated_archives() {
    std::mt19937_64 rng(991);
    for (const auto &name : backend_names()) {
        auto b = make_backend(name);
        b->build({raw(93, [](uint64_t i) { return i % 7 < 3; }),
                  raw(93, [](uint64_t i) { return i < 65; })},
                 93);
        const auto good = b->serialize();
        for (unsigned trial = 0; trial < 300; ++trial) {
            auto bytes = good;
            const size_t index = 40 + rng() % (bytes.size() - 40);
            bytes[index] ^= uint8_t(1U << (rng() % 8));
            auto loaded = make_backend(name);
            try {
                loaded->deserialize(bytes);
            } catch (const std::exception &) {
                continue;
            }
            check(loaded->serialize().size() == loaded->serialized_bytes(),
                  name + " mutated archive size");
            for (size_t id = 0; id < 2; ++id) {
                auto r = loaded->query_or({id});
                auto words = r->to_words();
                check(words.size() == 2 && !(words.back() >> 29),
                      name + " accepted mutation padding");
                check(r->cardinality() == cardinality(words),
                      name + " accepted mutation cardinality");
                check(loaded->query_and({id, id})->to_words() == words,
                      name + " accepted mutation AND identity");
                check(loaded->query_or({id, id})->to_words() == words,
                      name + " accepted mutation OR identity");
            }
        }
    }
}

} // namespace
int main() {
    try {
        golden_examples();
        boundaries();
        settings_and_random();
        exhaustive_bbc_bytes();
        invalid_data();
        mixed_sbh_regions();
        large_counters();
        mutated_archives();
        std::cout << "PASS: " << checks << " checks across all nine backends\n";
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
