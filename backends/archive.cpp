#include "backends/archive.hpp"
#include "common/bits.hpp"
#include <algorithm>

namespace bench {
uint32_t format_id(const std::string &name) {
    const std::vector<std::string> names{"raw",   "wah", "roaring", "ewah", "concise",
                                         "plwah", "val", "sbh",     "bbc"};
    const auto it = std::find(names.begin(), names.end(), name);
    if (it == names.end())
        throw std::runtime_error("unknown representation: " + name);
    return static_cast<uint32_t>(it - names.begin() + 1);
}
Bytes pack_archive(const DatasetArchive &a) {
    Bytes out{'B', 'M', 'C', 'F', '0', '0', '0', '1'};
    put_le(out, a.format, 4);
    put_le(out, 0, 4); // reserved, must be zero in version 1
    put_le(out, a.num_bits, 8);
    put_le(out, a.payloads.size(), 8);
    for (const auto &p : a.payloads) {
        put_le(out, p.size(), 8);
        out.insert(out.end(), p.begin(), p.end());
    }
    return out;
}
DatasetArchive unpack_archive(const Bytes &bytes, uint32_t expected) {
    ByteReader in(bytes);
    for (char c : std::string("BMCF0001"))
        if (in.get(1) != static_cast<uint8_t>(c))
            throw std::runtime_error("unknown archive magic/version");
    DatasetArchive a;
    a.format = static_cast<uint32_t>(in.get(4));
    if (a.format != expected)
        throw std::runtime_error("archive representation mismatch");
    if (in.get(4) != 0)
        throw std::runtime_error("nonzero archive reserved field");
    a.num_bits = in.get(8);
    if (a.num_bits > (uint64_t(1) << 32))
        throw std::runtime_error("archive universe exceeds 2^32 positions");
    const uint64_t count = in.get(8);
    if (count > in.remaining() / 8)
        throw std::runtime_error("invalid archive bitmap count");
    a.payloads.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; ++i) {
        const uint64_t len = in.get(8);
        a.payloads.push_back(in.take(len));
    }
    in.require_end();
    return a;
}
void validate_inputs(const std::vector<std::vector<uint64_t>> &bitmaps, uint64_t n) {
    if (n > (uint64_t(1) << 32))
        throw std::runtime_error("supported universe is 0..2^32 bits");
    const size_t words = words_for_bits(n);
    for (const auto &b : bitmaps) {
        if (b.size() != words)
            throw std::runtime_error("bitmap word count does not match num_bits");
        if (n % 64 && !b.empty() && (b.back() >> (n % 64)))
            throw std::runtime_error("nonzero padding beyond num_bits");
    }
}
void validate_ids(const std::vector<size_t> &ids, size_t count) {
    if (ids.empty())
        throw std::runtime_error("query has no operands");
    for (auto id : ids)
        if (id >= count)
            throw std::runtime_error("query bitmap index out of range");
}
} // namespace bench
