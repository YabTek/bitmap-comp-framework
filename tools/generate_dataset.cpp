
#include "common/bitmap_io.hpp"
#include "common/bits.hpp"
#include "common/cli.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using Label = std::uint8_t;
using RNG = std::mt19937_64;

// 0 = neither, 1 = A only, 2 = B only, 3 = both.
// Both bitmaps have k set bits and exactly t shared set bits.
void assign_membership(
    std::vector<Label>& data,
    std::uint64_t k,
    std::uint64_t t
) {
    const std::uint64_t n = data.size();

    if (k > n || t > k || 2 * k - t > n)
        throw std::runtime_error("invalid counts for membership generation");

    auto it = data.begin();

    it = std::fill_n(it, static_cast<std::size_t>(t), Label(3));
    it = std::fill_n(it, static_cast<std::size_t>(k - t), Label(1));
    std::fill_n(it, static_cast<std::size_t>(k - t), Label(2));
}

// Arrange membership labels into consecutive clusters.
void cluster_labels(
    std::vector<Label>& labels,
    std::uint64_t clusters,
    RNG& rng
) {
    std::uint64_t count[4] = {};

    for (Label l : labels)
        ++count[l];

    std::vector<std::pair<Label, std::uint64_t>> pieces;

    for (unsigned l = 0; l < 4; ++l) {
        auto pieces_for_label = std::min(clusters, count[l]);

        if (!pieces_for_label)
            continue;

        auto base = count[l] / pieces_for_label;
        auto extra = count[l] % pieces_for_label;

        for (std::uint64_t i = 0; i < pieces_for_label; ++i) {
            pieces.emplace_back(
                static_cast<Label>(l),
                base + (i < extra ? 1 : 0)
            );
        }
    }

    std::shuffle(pieces.begin(), pieces.end(), rng);

    std::size_t at = 0;

    for (const auto& part : pieces) {
        std::fill(
            labels.begin() + at,
            labels.begin() + at + part.second,
            part.first
        );

        at += static_cast<std::size_t>(part.second);
    }
}

// Mixed regions: different blocks have different local densities.
// Alternate scattered and clustered block generators.
void make_mixed(
    std::vector<Label>& labels,
    std::uint64_t k,
    std::uint64_t t,
    std::uint64_t blocks,
    std::uint64_t clusters,
    double contrast,
    RNG& rng
) {
    const std::uint64_t n = labels.size();

    if (blocks > n)
        throw std::runtime_error("blocks cannot exceed N");

    std::vector<std::uint64_t> lens(blocks);
    std::vector<std::uint64_t> counts(blocks);
    std::vector<std::uint64_t> intersects(blocks);

    std::uint64_t counted = 0;
    double global_density = static_cast<double>(k) / n;

    for (std::uint64_t i = 0; i < blocks; ++i) {
        lens[i] = n * (i + 1) / blocks - n * i / blocks;

        auto local_density = std::max(
            0.0,
            std::min(
                1.0,
                global_density + (i % 2 ? contrast : -contrast)
            )
        );

        counts[i] = std::min(
            lens[i],
            static_cast<std::uint64_t>(
                std::llround(lens[i] * local_density)
            )
        );

        counted += counts[i];
    }

    // Adjust integer rounding to preserve total cardinality.
    while (counted != k) {
        bool changed = false;

        for (std::uint64_t i = 0; i < blocks && counted != k; ++i) {
            if (counted < k && counts[i] < lens[i]) {
                ++counts[i];
                ++counted;
                changed = true;
            } else if (counted > k && counts[i] > 0) {
                --counts[i];
                --counted;
                changed = true;
            }
        }

        if (!changed)
            throw std::runtime_error("cannot distribute mixed block counts");
    }

    std::uint64_t min_t = 0;
    std::uint64_t max_t = 0;

    for (std::uint64_t i = 0; i < blocks; ++i) {
        intersects[i] = 2 * counts[i] > lens[i]
            ? 2 * counts[i] - lens[i]
            : 0;

        min_t += intersects[i];
        max_t += counts[i];
    }

    if (t < min_t || t > max_t) {
        throw std::runtime_error(
            "mixed local densities require overlap between " +
            std::to_string(min_t) + " and " + std::to_string(max_t)
        );
    }

    std::uint64_t extra = t - min_t;
    const std::uint64_t capacity = max_t - min_t;
    std::uint64_t distributed = 0;

    for (std::uint64_t i = 0; i < blocks && capacity; ++i) {
        auto room = counts[i] - intersects[i];

        auto add = static_cast<std::uint64_t>(
            std::floor(
                static_cast<long double>(extra) * room / capacity
            )
        );

        intersects[i] += add;
        distributed += add;
    }

    for (
        std::uint64_t i = 0;
        distributed < extra;
        i = (i + 1) % blocks
    ) {
        if (intersects[i] < counts[i]) {
            ++intersects[i];
            ++distributed;
        }
    }

    std::size_t at = 0;

    for (std::uint64_t i = 0; i < blocks; ++i) {
        std::vector<Label> part(
            static_cast<std::size_t>(lens[i])
        );

        assign_membership(part, counts[i], intersects[i]);

        if (i % 2 == 0)
            std::shuffle(part.begin(), part.end(), rng);
        else
            cluster_labels(part, clusters, rng);

        std::copy(part.begin(), part.end(), labels.begin() + at);
        at += part.size();
    }
}

// Period length for the fifth-format focused experiments.
std::uint64_t period_for(const std::string& format) {
    if (format == "plwah" || format == "concise")
        return 248;

    if (format == "val")
        return 240;

    if (format == "sbh")
        return 1792;

    if (format == "bbc")
        return 128;

    throw std::runtime_error(
        "focus format must be plwah, concise, val, sbh or bbc"
    );
}

// Generate A with a pattern designed to test the selected format.
void make_focus_a(
    std::vector<std::uint64_t>& a,
    std::uint64_t n,
    const std::string& format,
    const std::string& which,
    std::uint64_t shift
) {
    const auto period = period_for(format);

    if (n % period != 0)
        throw std::runtime_error(
            "focused N must be a multiple of " +
            std::to_string(period)
        );

    if (format == "plwah" || format == "concise") {
        if (which != "eligible" && which != "ineligible")
            throw std::runtime_error(
                "use focus-case=eligible or ineligible"
            );
    } else if (which != "aligned" && which != "shifted") {
        throw std::runtime_error(
            "use focus-case=aligned or shifted"
        );
    }

    if (which == "aligned" || which == "eligible")
        shift = 0;

    for (std::uint64_t base = 0; base < n; base += period) {
        if (format == "plwah") {
            // Eligible: single exceptions after zero fills.
            // Ineligible: two exceptions in one mixed group.
            bench::set_bit(a, base + 93);
            bench::set_bit(
                a,
                base + (which == "eligible" ? 217 : 94)
            );
        } else if (format == "concise") {
            // Eligible: exceptions at the beginning of fills.
            // Ineligible: two exceptions in one mixed group.
            bench::set_bit(a, base + 30);
            bench::set_bit(
                a,
                base + (which == "eligible" ? 154 : 29)
            );
        } else {
            for (std::uint64_t j = 0; j < period; ++j) {
                bool one = false;

                if (format == "val") {
                    one = (j >= 60 && j < 120) ||
                          (j >= 120 && j < 180 && j % 15 < 2);
                } else if (format == "sbh") {
                    one = j >= 896;
                } else if (format == "bbc") {
                    one = (j >= 32 && j < 64) ||
                          (j >= 64 && (j % 8) % 2 == 0);
                }

                if (one) {
                    bench::set_bit(
                        a,
                        base + (j + shift) % period
                    );
                }
            }
        }
    }
}

// Generate B with exactly t shared bits and k-t nonshared bits.
void make_focus_b(
    const std::vector<std::uint64_t>& a,
    std::vector<std::uint64_t>& b,
    std::uint64_t n,
    std::uint64_t k,
    std::uint64_t t,
    RNG& rng
) {
    std::vector<std::uint32_t> in_a;
    std::vector<std::uint32_t> out_a;

    in_a.reserve(static_cast<std::size_t>(k));
    out_a.reserve(static_cast<std::size_t>(n - k));

    for (std::uint64_t i = 0; i < n; ++i) {
        if ((a[i / 64] >> (i % 64)) & 1ULL)
            in_a.push_back(static_cast<std::uint32_t>(i));
        else
            out_a.push_back(static_cast<std::uint32_t>(i));
    }

    std::shuffle(in_a.begin(), in_a.end(), rng);
    std::shuffle(out_a.begin(), out_a.end(), rng);

    for (std::uint64_t i = 0; i < t; ++i)
        bench::set_bit(b, in_a[static_cast<std::size_t>(i)]);

    for (std::uint64_t i = 0; i < k - t; ++i)
        bench::set_bit(b, out_a[static_cast<std::size_t>(i)]);
}

} // namespace

int main(int argc, char** argv) {
    try {
        bench::Arguments args(
            argc,
            argv,
            {
                "out", "num-bits", "density", "pattern",
                "seed", "overlap", "clusters", "blocks",
                "mixed-contrast", "focus-format", "focus-case",
                "shift", "boundary-case", "force"
            }
        );

        if (args.help) {
            std::cout
                << "generate_dataset --out=DIR --num-bits=N "
                << "--pattern=NAME [options]\n"
                << "General patterns: sparse_scattered, "
                << "sparse_clustered, dense_scattered, "
                << "dense_clustered, mixed\n"
                << "Required: --density=P (0..1)\n"
                << "Optional: --seed=42 --overlap=T "
                << "--clusters=8 --blocks=8\n"
                << " --mixed-contrast=0.20 --force=0\n"
                << "Focus: --pattern=focused "
                << "--focus-format=plwah|concise|val|sbh|bbc\n"
                << " --focus-case=eligible|ineligible|aligned|shifted "
                << "--shift=7\n"
                << "Boundary: --pattern=boundary "
                << "--boundary-case=shifted|identical|disjoint\n";

            return 0;
        }

        const auto output = args.get("out");
        const auto pattern = args.get("pattern");
        const auto n = args.number("num-bits", 0);
        const auto seed = args.number("seed", 42);
        const auto clusters = args.number("clusters", 8);
        const auto blocks = args.number("blocks", 8);
        const auto focus_format = args.get("focus-format");
        const auto focus_case = args.get("focus-case");
        const auto shift = args.number("shift", 7);

        const auto contrast = bench::parse_real(
            args.get("mixed-contrast", "0.20"),
            "--mixed-contrast"
        );

        if (
            output.empty() ||
            n < 1 ||
            n > (std::uint64_t(1) << 32)
        ) {
            throw std::runtime_error(
                "require --out and 1 <= --num-bits <= 2^32"
            );
        }

        if (
            !clusters ||
            !blocks ||
            blocks > n ||
            contrast < 0.0 ||
            contrast > 0.5
        ) {
            throw std::runtime_error(
                "invalid cluster, block or contrast setting"
            );
        }

        const bool focused = pattern == "focused";
        const bool boundary = pattern == "boundary";

        std::vector<std::uint64_t> a(
            bench::words_for_bits(n), 0
        );

        std::vector<std::uint64_t> b(
            bench::words_for_bits(n), 0
        );

        RNG rng(seed);

        std::uint64_t k = 0;
        std::uint64_t t = 0;
        double density = 0;

        if (boundary) {
            const std::string case_name = args.get(
                "boundary-case", "shifted"
            );

            if (
                case_name != "shifted" &&
                case_name != "identical" &&
                case_name != "disjoint"
            ) {
                throw std::runtime_error(
                    "unsupported boundary case"
                );
            }

            const std::vector<std::uint64_t> positions = {
                0, 30, 31, 32, 62, 63, 64, 65,
                65534, 65535, 65536, n - 1
            };

            for (auto p : positions)
                if (p < n)
                    bench::set_bit(a, p);

            k = bench::cardinality(a);

            if (case_name == "identical") {
                b = a;
            } else if (case_name == "shifted") {
                for (std::uint64_t i = 0; i < n; ++i) {
                    if ((a[i / 64] >> (i % 64)) & 1ULL)
                        bench::set_bit(b, (i + 1) % n);
                }
            } else {
                std::uint64_t added = 0;

                for (
                    std::uint64_t i = 0;
                    i < n && added < k;
                    ++i
                ) {
                    if (((a[i / 64] >> (i % 64)) & 1ULL) == 0) {
                        bench::set_bit(b, i);
                        ++added;
                    }
                }

                if (added != k)
                    throw std::runtime_error(
                        "universe too small for disjoint boundary set"
                    );
            }

            t = 0;

            for (std::size_t i = 0; i < a.size(); ++i) {
                t += static_cast<std::uint64_t>(
                    bench::popcount64(a[i] & b[i])
                );
            }

            density = static_cast<double>(k) / n;
        } else if (focused) {
            make_focus_a(a, n, focus_format, focus_case, shift);

            k = bench::cardinality(a);
            density = static_cast<double>(k) / n;

            auto minimum = 2 * k > n ? 2 * k - n : 0ULL;

            t = args.has("overlap")
                ? args.number("overlap", 0)
                : minimum + (k - minimum) / 2;

            if (t < minimum || t > k)
                throw std::runtime_error(
                    "focused overlap infeasible"
                );

            make_focus_b(a, b, n, k, t, rng);
        } else {
            density = bench::parse_real(
                args.get("density", ""),
                "--density"
            );

            if (density < 0 || density > 1)
                throw std::runtime_error(
                    "density must be in [0,1]"
                );

            if (
                pattern != "sparse_scattered" &&
                pattern != "sparse_clustered" &&
                pattern != "dense_scattered" &&
                pattern != "dense_clustered" &&
                pattern != "mixed"
            ) {
                throw std::runtime_error(
                    "unknown general pattern"
                );
            }

            if (
                pattern.rfind("sparse_", 0) == 0 &&
                density > 0.5
            ) {
                throw std::runtime_error(
                    "sparse requires density <= 0.5"
                );
            }

            if (
                pattern.rfind("dense_", 0) == 0 &&
                density < 0.5
            ) {
                throw std::runtime_error(
                    "dense requires density >= 0.5"
                );
            }

            k = static_cast<std::uint64_t>(
                std::llround(density * static_cast<double>(n))
            );

            auto lower = 2 * k > n ? 2 * k - n : 0ULL;
            auto upper = k;

            t = args.has("overlap")
                ? args.number("overlap", 0)
                : lower + (upper - lower) / 2;

            if (t < lower || t > upper)
                throw std::runtime_error(
                    "overlap outside globally feasible range"
                );

            std::vector<Label> labels(
                static_cast<std::size_t>(n), 0
            );

            if (pattern == "mixed") {
                make_mixed(
                    labels, k, t, blocks,
                    clusters, contrast, rng
                );
            } else {
                assign_membership(labels, k, t);

                if (
                    pattern.find("scattered") !=
                    std::string::npos
                ) {
                    std::shuffle(
                        labels.begin(), labels.end(), rng
                    );
                } else {
                    cluster_labels(labels, clusters, rng);
                }
            }

            for (std::uint64_t i = 0; i < n; ++i) {
                if (labels[static_cast<std::size_t>(i)] & 1)
                    bench::set_bit(a, i);

                if (labels[static_cast<std::size_t>(i)] & 2)
                    bench::set_bit(b, i);
            }
        }

        bench::mask_tail(a, n);
        bench::mask_tail(b, n);

        std::uint64_t intersection = 0;

        for (std::size_t i = 0; i < a.size(); ++i) {
            intersection += static_cast<std::uint64_t>(
                bench::popcount64(a[i] & b[i])
            );
        }

        if (
            bench::cardinality(a) != k ||
            bench::cardinality(b) != k ||
            intersection != t
        ) {
            throw std::runtime_error(
                "internal cardinality/overlap mismatch"
            );
        }

        std::filesystem::path path(output);

        if (
            !args.boolean("force", false) &&
            std::filesystem::exists(path / "metadata.txt")
        ) {
            throw std::runtime_error(
                "dataset already exists; use --force=1"
            );
        }

        bench::DatasetMetadata meta;

        meta.num_bits = n;
        meta.num_bitmaps = 2;

        meta.kind = focused
            ? "focus_" + focus_format + "_" + focus_case
            : boundary
                ? "boundary_" + args.get(
                    "boundary-case", "shifted"
                  )
                : pattern;

        meta.density = static_cast<double>(k) / n;
        meta.seed = seed;
        meta.seed_present = true;

        meta.params =
            "k=" + std::to_string(k) +
            ";t=" + std::to_string(t) +
            ";clusters=" + std::to_string(clusters) +
            ";blocks=" + std::to_string(blocks) +
            ";contrast=" + std::to_string(contrast) +
            ";focus=" + focus_format +
            ";case=" + focus_case +
            ";shift=" + std::to_string(shift);

        bench::write_metadata(output, meta);

        bench::write_bitmap(
            (path / bench::bitmap_filename(0)).string(), a
        );

        bench::write_bitmap(
            (path / bench::bitmap_filename(1)).string(), b
        );

        std::cout
            << "Created " << output
            << " N=" << n
            << " k=" << k
            << " t=" << t
            << " seed=" << seed
            << " kind=" << meta.kind
            << '\n';

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
