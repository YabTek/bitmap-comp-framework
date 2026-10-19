#pragma once
#include "common/binary.hpp"
#include <string>

namespace bench {
// A dataset archive: magic/version, representation ID, logical length, number
// of bitmaps, then length-prefixed format payloads. All bytes count toward
// size.
struct DatasetArchive {
    uint32_t format = 0;
    uint64_t num_bits = 0;
    std::vector<Bytes> payloads;
};
uint32_t format_id(const std::string &name);
Bytes pack_archive(const DatasetArchive &archive);
DatasetArchive unpack_archive(const Bytes &bytes, uint32_t expected_format);
void validate_inputs(const std::vector<std::vector<uint64_t>> &bitmaps, uint64_t num_bits);
void validate_ids(const std::vector<size_t> &ids, size_t count);
} // namespace bench
