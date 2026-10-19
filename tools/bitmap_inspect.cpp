// Inspect one already supplied bitmap; this is not a dataset generator.
#include "backends/archive.hpp"
#include "backends/factory.hpp"
#include "common/bitmap_io.hpp"
#include "common/cli.hpp"
#include <iomanip>
#include <iostream>

int main(int argc, char **argv) {
    try {
        bench::Arguments args(argc, argv,
                              {"backend", "dataset", "bitmap", "max-bytes", "max-positions",
                               "val-segment-bits", "sbh-groups"});
        if (args.help) {
            std::cout << "bitmap_inspect --backend=NAME --dataset=DIR [--bitmap=0] "
                         "[--max-bytes=128] "
                         "[--max-positions=32] [--val-segment-bits=15|30|60|auto] "
                         "[--sbh-groups=128]\n";
            return 0;
        }
        if (args.get("backend").empty() || args.get("dataset").empty())
            throw std::runtime_error("backend and dataset are required");
        bench::DatasetMetadata metadata;
        auto data = bench::load_dataset(args.get("dataset"), metadata);
        const uint64_t id = args.number("bitmap", 0);
        if (id >= data.size())
            throw std::runtime_error("bitmap index out of range");
        bench::CodecOptions options;
        const auto val = args.get("val-segment-bits", "15");
        const auto width = val == "auto" ? 0 : bench::parse_unsigned(val, "VAL segment bits");
        if (width != 0 && width != 15 && width != 30 && width != 60)
            throw std::runtime_error("invalid VAL segment size");
        const auto sbh = args.number("sbh-groups", 128);
        if (!sbh || sbh > 4095)
            throw std::runtime_error("invalid SBH group count");
        options.val_segment_bits = static_cast<unsigned>(width);
        options.sbh_groups = static_cast<unsigned>(sbh);
        auto backend = bench::make_backend(args.get("backend"), options);
        backend->build({data.at(static_cast<size_t>(id))}, metadata.num_bits);
        const auto value = backend->query_or({0});
        const auto a =
            bench::unpack_archive(backend->serialize(), bench::format_id(backend->name()));
        const auto &bytes = a.payloads.at(0);
        const auto stats = backend->statistics();
        std::cout << "backend=" << backend->name() << " num_bits=" << metadata.num_bits
                  << " cardinality=" << value->cardinality()
                  << " payload_bytes=" << backend->payload_bytes()
                  << " saved_bytes=" << backend->serialized_bytes() << '\n';
        std::cout << "literals=" << stats.literal_units << " fills=" << stats.fill_units
                  << " exceptions=" << stats.exception_units << " controls=" << stats.control_units
                  << " array_containers=" << stats.array_containers
                  << " bitmap_containers=" << stats.bitmap_containers
                  << " run_containers=" << stats.run_containers << '\n';
        const uint64_t max_positions = args.number("max-positions", 32);
        std::cout << "present_positions:";
        uint64_t shown = 0;
        for (size_t w = 0; w < data[id].size() && shown < max_positions; ++w) {
            auto bits = data[id][w];
            while (bits && shown < max_positions) {
                std::cout << ' ' << (w * 64 + static_cast<size_t>(bench::ctz64(bits)));
                bits &= bits - 1;
                ++shown;
            }
        }
        if (shown < value->cardinality())
            std::cout << " ...";
        std::cout << "\npayload_hex (file order; multi-byte words are little-endian):\n";
        const uint64_t max_bytes = args.number("max-bytes", 128);
        for (size_t i = 0; i < bytes.size() && i < max_bytes; ++i) {
            if (i % 16 == 0)
                std::cout << std::hex << std::setfill('0') << std::setw(6) << i << ": ";
            std::cout << std::setw(2) << unsigned(bytes[i]) << ' ';
            if (i % 16 == 15 || i + 1 == bytes.size() || i + 1 == max_bytes)
                std::cout << '\n';
        }
        if (bytes.size() > max_bytes)
            std::cout << "... (output truncated)\n";
    } catch (const std::exception &e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
