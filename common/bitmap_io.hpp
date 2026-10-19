#pragma once
// Reading and writing datasets in the on-disk format described in the README.
#include "common/binary.hpp"
#include "common/bits.hpp"

#include <string>
#include <vector>

namespace bench {

struct DatasetMetadata {
    uint64_t num_bits = 0;
    size_t num_bitmaps = 0;
    size_t word_bits = 64;
    std::string format = "raw_uint64_le";

    // Optional, purely descriptive: they are copied into the result CSV so that
    // runs can be grouped during analysis. `kind` is the name of the
    // distribution the generator used ("uniform", "clustered", ...), `params`
    // is a free-form string for whatever else that generator was given. A
    // dataset without them still loads.
    std::string kind;
    std::string params;
    double density = -1.0; // -1 => not recorded
    uint64_t seed = 0;
    bool seed_present = false;
};

DatasetMetadata read_metadata(const std::string &dir);
void write_metadata(const std::string &dir, const DatasetMetadata &meta);

std::vector<uint64_t> read_bitmap(const std::string &path, size_t num_words);
void write_bitmap(const std::string &path, const std::vector<uint64_t> &words);

std::vector<std::vector<uint64_t>> load_dataset(const std::string &dir, DatasetMetadata &out_meta);

// "bitmap_000.bin", "bitmap_001.bin", ...
std::string bitmap_filename(size_t index);
Bytes read_bytes(const std::string &path);
void write_bytes(const std::string &path, const Bytes &bytes);

} // namespace bench
