// Dataset/query I/O, verification, output counting and statistics stay outside
// query timings. Allocation of the materialized result is included; releasing
// the returned result is not. Build samples use fresh backend instances.
#include "backends/archive.hpp"
#include "backends/factory.hpp"
#include "backends/raw_backend.hpp"
#include "common/bitmap_io.hpp"
#include "common/cli.hpp"
#include "common/query.hpp"
#include "common/timing.hpp"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

#ifndef BENCH_VERSION
#define BENCH_VERSION "development"
#endif
#ifndef BENCH_COMPILER
#define BENCH_COMPILER "unknown"
#endif
#ifndef BENCH_BUILD_TYPE
#define BENCH_BUILD_TYPE "unknown"
#endif

namespace {
std::string leaf(const std::string &s) {
    auto path = std::filesystem::path(s).lexically_normal();
    if (path.filename().empty())
        path = path.parent_path();
    return path.filename().string();
}
bench::QueryResultPtr run(const bench::BitmapBackend &b, const bench::Query &q) {
    return q.op == bench::QueryOp::And ? b.query_and(q.ids) : b.query_or(q.ids);
}
void usage() {
    std::cout << "bitmap_bench --backend=NAME --dataset=DIR --queries=FILE --out=CSV\n"
                 "  backends: raw wah ewah roaring concise plwah val sbh bbc\n"
                 "  --repetitions=5 --warmup=1 --build-repetitions=3 --verify=1\n"
                 "  --val-segment-bits=15|30|60|auto  --sbh-groups=128\n"
                 "  --save=ARCHIVE --load=ARCHIVE (optional; load replaces "
                 "construction)\n"
                 "  --list-backends --version --help\n";
}
void stats_csv(std::ostream &out, const bench::RepresentationStats &s) {
    out << s.literal_units << ',' << s.fill_units << ',' << s.exception_units << ','
        << s.control_units << ',' << s.array_containers << ',' << s.bitmap_containers << ','
        << s.run_containers;
}
} // namespace
int main(int argc, char **argv) {
    try {
        bench::Arguments args(argc, argv,
                              {"backend", "dataset", "queries", "out", "repetitions", "warmup",
                               "build-repetitions", "verify", "val-segment-bits", "sbh-groups",
                               "save", "load", "list-backends", "version"},
                              {"list-backends", "version"});
        if (args.help) {
            usage();
            return 0;
        }
        if (args.has("version")) {
            std::cout << "bitmap-comp-framework " << BENCH_VERSION << "; " << BENCH_COMPILER << "; "
                      << BENCH_BUILD_TYPE << '\n';
            return 0;
        }
        if (args.has("list-backends")) {
            for (const auto &n : bench::backend_names())
                std::cout << n << '\n';
            return 0;
        }
        const auto name = args.get("backend"), dataset = args.get("dataset"),
                   query_file = args.get("queries"), output = args.get("out");
        if (name.empty() || dataset.empty() || query_file.empty() || output.empty()) {
            usage();
            return 2;
        }
        const uint64_t reps = args.number("repetitions", 5), warmup = args.number("warmup", 1),
                       build_reps = args.number("build-repetitions", 3);
        if (!reps || !build_reps)
            throw std::runtime_error("repetitions and build-repetitions must be positive");
        const bool verify = args.boolean("verify", true);
        bench::CodecOptions options;
        const auto val = args.get("val-segment-bits", "15");
        const auto width = val == "auto" ? 0 : bench::parse_unsigned(val, "VAL segment size");
        if (width != 0 && width != 15 && width != 30 && width != 60)
            throw std::runtime_error("VAL segment size must be 15, 30, 60 or auto");
        options.val_segment_bits = static_cast<unsigned>(width);
        const auto sbh = args.number("sbh-groups", 128);
        if (!sbh || sbh > 4095)
            throw std::runtime_error("sbh-groups must be in 1..4095");
        options.sbh_groups = static_cast<unsigned>(sbh);

        bench::DatasetMetadata meta;
        const auto data = bench::load_dataset(dataset, meta);
        const auto queries = bench::read_queries(query_file);
        for (const auto &q : queries)
            bench::validate_ids(q.ids, data.size());
        bench::RawBackend reference;
        reference.build(data, meta.num_bits);
        std::unique_ptr<bench::BitmapBackend> backend;
        std::vector<double> build_samples;
        double build_ms = 0, load_ms = 0;
        if (args.has("load")) {
            const auto bytes = bench::read_bytes(args.get("load"));
            const auto archive = bench::unpack_archive(bytes, bench::format_id(name));
            if (archive.num_bits != meta.num_bits || archive.payloads.size() != data.size())
                throw std::runtime_error("loaded archive does not match dataset dimensions");
            backend = bench::make_backend(name, options);
            bench::Timer timer;
            backend->deserialize(bytes);
            load_ms = timer.elapsed_ms();
        } else {
            for (uint64_t r = 0; r < build_reps; ++r) {
                auto fresh = bench::make_backend(name, options);
                bench::Timer timer;
                fresh->build(data, meta.num_bits);
                build_samples.push_back(timer.elapsed_ms());
                backend = std::move(fresh);
            }
            auto sorted = build_samples;
            std::sort(sorted.begin(), sorted.end());
            const size_t m = sorted.size() / 2;
            build_ms = sorted.size() % 2 ? sorted[m] : (sorted[m - 1] + sorted[m]) / 2;
        }
        const auto bytes = backend->serialized_bytes(), payload = backend->payload_bytes();
        const auto raw_bytes = reference.serialized_bytes(),
                   raw_payload = reference.payload_bytes();
        const auto stats = backend->statistics();
        const double ratio = bytes ? double(raw_bytes) / double(bytes) : 0;
        if (args.has("save"))
            bench::write_bytes(args.get("save"), backend->serialize());

        std::unique_ptr<bench::BitmapBackend> restored;
        if (verify) {
            const auto archive = backend->serialize();
            if (archive.size() != bytes)
                throw std::runtime_error("saved size disagrees with actual archive");
            restored = bench::make_backend(name, options);
            restored->deserialize(archive);
            for (size_t i = 0; i < data.size(); ++i) {
                for (const auto *candidate : {backend.get(), restored.get()}) {
                    const auto value = candidate->query_or({i});
                    if (value->to_words() != data[i] ||
                        value->cardinality() != bench::cardinality(data[i]))
                        throw std::runtime_error(
                            "bitmap failed build/save/load verification at index " +
                            std::to_string(i));
                }
            }
        }
        const auto parent = std::filesystem::path(output).parent_path();
        if (!parent.empty())
            std::filesystem::create_directories(parent);
        std::ofstream csv(output);
        if (!csv)
            throw std::runtime_error("cannot write " + output);
        csv << std::setprecision(17);
        csv << "backend,dataset,kind,params,num_bits,num_bitmaps,density,seed,"
               "query_file,query_index,query_type,query_width,repetition,"
               "build_time_ms,query_time_ns,result_cardinality,raw_bytes,"
               "compressed_bytes,compression_ratio,"
               "raw_payload_bytes,payload_bytes,result_payload_bytes,query_ids,"
               "input_cardinalities,intersection_cardinality,"
               "build_repetitions,load_time_ms,input_mode,val_segment_bits,sbh_"
               "groups,verified,"
               "literal_units,fill_units,exception_units,control_units,array_"
               "containers,bitmap_containers,run_containers\n";
        const std::string dataset_name = leaf(dataset), query_name = leaf(query_file);
        // Archives carry their own selectors and region sizes. Do not label
        // loaded data with unused construction options from this invocation.
        const std::string val_label = name == "val" ? (args.has("load") ? "stored" : val) : "";
        const std::string sbh_label =
            name == "sbh" ? (args.has("load") ? "stored" : std::to_string(sbh)) : "";
        for (size_t qi = 0; qi < queries.size(); ++qi) {
            const auto &q = queries[qi];
            const auto expected = run(reference, q)->to_words();
            const auto intersection = reference.query_and(q.ids)->cardinality();
            std::ostringstream ids, cards;
            for (size_t j = 0; j < q.ids.size(); ++j) {
                if (j) {
                    ids << ';';
                    cards << ';';
                }
                ids << q.ids[j];
                cards << bench::cardinality(data[q.ids[j]]);
            }
            if (verify && run(*restored, q)->to_words() != expected)
                throw std::runtime_error("loaded archive query mismatch at query " +
                                         std::to_string(qi));
            for (uint64_t w = 0; w < warmup; ++w)
                (void)run(*backend, q);
            for (uint64_t r = 0; r < reps; ++r) {
                bench::Timer timer;
                const auto result = run(*backend, q);
                const int64_t ns = timer.elapsed_ns();
                const auto card = result->cardinality();
                if (verify &&
                    (result->to_words() != expected || card != bench::cardinality(expected)))
                    throw std::runtime_error("query verification mismatch at query " +
                                             std::to_string(qi));
                csv << backend->name() << ',' << bench::csv_field(dataset_name) << ','
                    << bench::csv_field(meta.kind) << ',' << bench::csv_field(meta.params) << ','
                    << meta.num_bits << ',' << data.size() << ',' << meta.density << ',';
                if (meta.seed_present)
                    csv << meta.seed;
                csv << ',' << bench::csv_field(query_name) << ',' << qi << ','
                    << bench::query_op_str(q.op) << ',' << q.ids.size() << ',' << r << ','
                    << build_ms << ',' << ns << ',' << card << ',' << raw_bytes << ',' << bytes
                    << ',' << ratio << ',' << raw_payload << ',' << payload << ','
                    << result->payload_bytes() << ',' << ids.str() << ',' << cards.str() << ','
                    << intersection << ',' << (args.has("load") ? 0 : build_reps) << ',' << load_ms
                    << ',' << (args.has("load") ? "load" : "build") << ',' << val_label << ','
                    << sbh_label << ',' << (verify ? 1 : 0) << ',';
                stats_csv(csv, stats);
                csv << '\n';
            }
        }
        csv.close();
        if (!csv)
            throw std::runtime_error("failed writing results: " + output);
        std::ofstream run_info(output + ".run.txt");
        run_info << "framework=" << BENCH_VERSION << "\ncompiler=" << BENCH_COMPILER
                 << "\nbuild_type=" << BENCH_BUILD_TYPE << "\nbackend=" << backend->name()
                 << "\nverified=" << verify << "\nwarmup=" << warmup << "\nrepetitions=" << reps
                 << "\ninput_mode=" << (args.has("load") ? "load" : "build")
                 << "\nbuild_samples_ms=" << std::setprecision(17);
        for (auto ms : build_samples)
            run_info << ms << ';';
        run_info << "\nval_segment_bits=" << val_label << "\nsbh_groups=" << sbh_label
                 << "\ndataset=" << std::filesystem::absolute(dataset).string()
                 << "\nqueries=" << std::filesystem::absolute(query_file).string()
                 << "\nallocation=materialized result allocation included; "
                    "returned-result destruction excluded\n";
        run_info.close();
        if (!run_info)
            throw std::runtime_error("failed writing run metadata: " + output + ".run.txt");
        std::cout << "backend=" << backend->name() << " queries=" << queries.size()
                  << " repetitions=" << reps << " saved_bytes=" << bytes
                  << " payload_bytes=" << payload << " -> " << output << '\n';
        if (verify)
            std::cout << "verify: 0 mismatches; input, save/load and query checks passed\n";
    } catch (const std::exception &e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
