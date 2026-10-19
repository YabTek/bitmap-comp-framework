#include "common/bitmap_io.hpp"
#include "common/cli.hpp"
#include "common/query.hpp"
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
using namespace bench;
namespace {
void check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
void rejects(const std::function<void()> &f) {
    try {
        f();
    } catch (const std::exception &) {
        return;
    }
    throw std::runtime_error("invalid input unexpectedly accepted");
}
void text(const std::string &path, const std::string &value) {
    write_bytes(path, Bytes(value.begin(), value.end()));
}
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc != 2)
            throw std::runtime_error("test requires a scratch directory");
        const std::string dir = argv[1];
        std::filesystem::create_directories(dir);
        DatasetMetadata meta;
        meta.num_bits = 65;
        meta.num_bitmaps = 2;
        meta.kind = "test";
        meta.params = "a,b=\"c\"";
        meta.density = 0.12345678912345678;
        meta.seed = 0;
        meta.seed_present = true;
        write_metadata(dir, meta);
        write_bitmap(dir + "/bitmap_000.bin", {0x0123456789abcdefULL, 1});
        write_bitmap(dir + "/bitmap_001.bin", {0, 0});
        DatasetMetadata read;
        const auto bitmaps = load_dataset(dir, read);
        check(bitmaps[0] == std::vector<uint64_t>({0x0123456789abcdefULL, 1}), "raw logical words");
        check(read.seed_present && read.seed == 0 && read.params == meta.params,
              "metadata identity");
        check(read.density == meta.density, "metadata numeric precision");
        check(read_bytes(dir + "/bitmap_000.bin") ==
                  Bytes({0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01, 1, 0, 0, 0, 0, 0, 0, 0}),
              "little-endian fixture");
        write_bitmap(dir + "/bitmap_001.bin", {0, 2});
        rejects([&] { load_dataset(dir, read); });
        write_bytes(dir + "/bitmap_001.bin", Bytes(15, 0));
        rejects([&] { load_dataset(dir, read); });
        write_bytes(dir + "/bitmap_001.bin", Bytes(17, 0));
        rejects([&] { load_dataset(dir, read); });
        const std::string valid = "num_bits=0\r\nnum_bitmaps=1\r\nword_bits="
                                  "64\r\nformat=raw_uint64_le\r\n";
        text(dir + "/metadata.txt", valid);
        write_bytes(dir + "/bitmap_000.bin", {});
        check(load_dataset(dir, read) == std::vector<std::vector<uint64_t>>{{}},
              "zero universe and CRLF");
        for (const auto &bad :
             {valid + "num_bits=1\n", valid + "density=NaN\n", valid + "density=1.1\n",
              std::string("num_bits=10\nnum_bitmaps=1\nword_bits=64\n"),
              std::string("num_bits=-1\nnum_bitmaps=1\nword_bits=64\nformat=raw_"
                          "uint64_le\n")}) {
            text(dir + "/metadata.txt", bad);
            rejects([&] { read_metadata(dir); });
        }
        const auto qp = dir + "/queries.txt";
        text(qp, " # comment\r\nAND 2 0 1\r\nOR 3 1 0 1\r\n");
        auto queries = read_queries(qp);
        check(queries.size() == 2 && queries[1].ids == std::vector<size_t>({1, 0, 1}),
              "queries and repeated IDs");
        write_queries(qp, queries);
        check(read_queries(qp)[0].ids == queries[0].ids, "query round trip");
        for (const auto *bad : {"AND 0\n", "AND 2 0\n", "AND 1 -1\n", "AND 1 0 extra\n",
                                "XOR 1 0\n", "# only comment\n"}) {
            text(qp, bad);
            rejects([&] { read_queries(qp); });
        }
        check(csv_field(meta.params) == "\"a,b=\"\"c\"\"\"", "CSV quoting");
        std::cout << "PASS: dataset byte order, metadata, padding and query validation\n";
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
