#include "common/bitmap_io.hpp"

#include "common/cli.hpp"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <set>
#include <stdexcept>

namespace bench {

namespace fs = std::filesystem;
namespace {
bool native_little_endian() {
    const uint16_t value = 1;
    return *reinterpret_cast<const unsigned char *>(&value) == 1;
}
} // namespace

std::string bitmap_filename(size_t index) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "bitmap_%03zu.bin", index);
    return std::string(buf);
}

DatasetMetadata read_metadata(const std::string &dir) {
    fs::path mp = fs::path(dir) / "metadata.txt";
    std::ifstream in(mp);
    if (!in)
        throw std::runtime_error("cannot open " + mp.string());
    DatasetMetadata m;
    std::set<std::string> seen;
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        auto eq = line.find('=');
        if (eq == std::string::npos)
            throw std::runtime_error("bad metadata line: " + line);
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));
        if (!seen.insert(key).second)
            throw std::runtime_error("duplicate metadata key: " + key);
        if (key == "num_bits")
            m.num_bits = parse_unsigned(val, key);
        else if (key == "num_bitmaps")
            m.num_bitmaps = static_cast<size_t>(parse_unsigned(val, key));
        else if (key == "word_bits")
            m.word_bits = static_cast<size_t>(parse_unsigned(val, key));
        else if (key == "format")
            m.format = val;
        else if (key == "kind")
            m.kind = val;
        else if (key == "params")
            m.params = val;
        else if (key == "density")
            m.density = parse_real(val, key);
        else if (key == "seed") {
            m.seed = parse_unsigned(val, key);
            m.seed_present = true;
        }
        // Unknown keys are ignored on purpose: a generator may record whatever
        // else it wants without breaking the reader.
    }
    for (const std::string key : {"num_bits", "num_bitmaps", "word_bits", "format"})
        if (!seen.count(key))
            throw std::runtime_error("missing metadata key: " + key);
    if (!m.num_bitmaps)
        throw std::runtime_error("num_bitmaps must be positive");
    if (m.num_bits > (uint64_t(1) << 32))
        throw std::runtime_error("num_bits exceeds supported 2^32-bit universe");
    if (seen.count("density") && (m.density < 0 || m.density > 1))
        throw std::runtime_error("density must be in [0,1]");
    if (m.word_bits != 64)
        throw std::runtime_error("only word_bits=64 supported");
    if (m.format != "raw_uint64_le")
        throw std::runtime_error("only format=raw_uint64_le supported");
    return m;
}

void write_metadata(const std::string &dir, const DatasetMetadata &meta) {
    fs::create_directories(dir);
    fs::path mp = fs::path(dir) / "metadata.txt";
    std::ofstream out(mp);
    if (!out)
        throw std::runtime_error("cannot write " + mp.string());
    out << std::setprecision(17);
    out << "num_bits=" << meta.num_bits << "\n";
    out << "num_bitmaps=" << meta.num_bitmaps << "\n";
    out << "word_bits=" << meta.word_bits << "\n";
    out << "format=" << meta.format << "\n";
    if (!meta.kind.empty())
        out << "kind=" << meta.kind << "\n";
    if (!meta.params.empty())
        out << "params=" << meta.params << "\n";
    if (meta.density >= 0.0)
        out << "density=" << meta.density << "\n";
    if (meta.seed_present)
        out << "seed=" << meta.seed << "\n";
    out.close();
    if (!out)
        throw std::runtime_error("failed writing " + mp.string());
}

std::vector<uint64_t> read_bitmap(const std::string &path, size_t num_words) {
    if (fs::file_size(path) != num_words * sizeof(uint64_t))
        throw std::runtime_error("bitmap file size mismatch: " + path);
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot open " + path);
    std::vector<uint64_t> words(num_words);
    const size_t bytes = num_words * sizeof(uint64_t);
    if (bytes &&
        !in.read(reinterpret_cast<char *>(words.data()), static_cast<std::streamsize>(bytes)))
        throw std::runtime_error("short read in " + path);
    if (!native_little_endian())
        for (auto &word : words) {
            const auto *b = reinterpret_cast<const unsigned char *>(&word);
            uint64_t decoded = 0;
            for (unsigned i = 0; i < 8; ++i)
                decoded |= uint64_t(b[i]) << (8 * i);
            word = decoded;
        }
    return words;
}

void write_bitmap(const std::string &path, const std::vector<uint64_t> &words) {
    std::ofstream out(path, std::ios::binary);
    if (!out)
        throw std::runtime_error("cannot write " + path);
    if (native_little_endian()) {
        if (!words.empty())
            out.write(reinterpret_cast<const char *>(words.data()),
                      static_cast<std::streamsize>(words.size() * sizeof(uint64_t)));
    } else
        for (auto word : words) {
            unsigned char b[8];
            for (unsigned i = 0; i < 8; ++i)
                b[i] = static_cast<unsigned char>(word >> (8 * i));
            out.write(reinterpret_cast<const char *>(b), 8);
        }
    out.close();
    if (!out)
        throw std::runtime_error("failed writing " + path);
}

std::vector<std::vector<uint64_t>> load_dataset(const std::string &dir, DatasetMetadata &out_meta) {
    out_meta = read_metadata(dir);
    size_t nw = words_for_bits(out_meta.num_bits);
    std::vector<std::vector<uint64_t>> bms;
    bms.reserve(out_meta.num_bitmaps);
    for (size_t i = 0; i < out_meta.num_bitmaps; ++i) {
        fs::path p = fs::path(dir) / bitmap_filename(i);
        bms.emplace_back(read_bitmap(p.string(), nw));
    }
    // Padding is a data error; do not silently repair students' generators.
    for (const auto &b : bms)
        if (out_meta.num_bits % 64 && !b.empty() && (b.back() >> (out_meta.num_bits % 64)))
            throw std::runtime_error("nonzero input padding beyond num_bits");
    return bms;
}

Bytes read_bytes(const std::string &path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
        throw std::runtime_error("cannot open " + path);
    const auto end = in.tellg();
    if (end < 0)
        throw std::runtime_error("cannot determine file size: " + path);
    Bytes bytes(static_cast<size_t>(end));
    in.seekg(0);
    if (!bytes.empty() && !in.read(reinterpret_cast<char *>(bytes.data()),
                                   static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("short read in " + path);
    return bytes;
}
void write_bytes(const std::string &path, const Bytes &bytes) {
    const auto parent = fs::path(path).parent_path();
    if (!parent.empty())
        fs::create_directories(parent);
    std::ofstream out(path, std::ios::binary);
    if (!out)
        throw std::runtime_error("cannot write " + path);
    if (!bytes.empty())
        out.write(reinterpret_cast<const char *>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    out.close();
    if (!out)
        throw std::runtime_error("failed writing " + path);
}

} // namespace bench
