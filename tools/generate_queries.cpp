#include "common/cli.hpp"
#include "common/query.hpp"
#include <iostream>
#include <numeric>
#include <random>

int main(int argc, char **argv) {
    try {
        bench::Arguments args(
            argc, argv, {"out", "num-bitmaps", "num-queries", "min-width", "max-width", "seed"});
        const auto out = args.get("out");
        const uint64_t count = args.number("num-bitmaps", 0),
                       queries = args.number("num-queries", 200);
        const uint64_t low = args.number("min-width", 2), high = args.number("max-width", 5);
        if (args.help) {
            std::cout << "generate_queries --out=PATH --num-bitmaps=N [--num-queries=200] "
                         "[--min-width=2] [--max-width=5] [--seed=43]\n";
            return 0;
        }
        if (out.empty() || !count || !queries || !low || low > high || high > count)
            throw std::runtime_error("require out, positive bitmap/query counts and "
                                     "1 <= min-width <= max-width <= num-bitmaps");
        std::mt19937_64 rng(args.number("seed", 43));
        std::uniform_int_distribution<size_t> width(static_cast<size_t>(low),
                                                    static_cast<size_t>(high));
        std::bernoulli_distribution op(0.5);
        std::vector<size_t> pool(static_cast<size_t>(count));
        std::iota(pool.begin(), pool.end(), 0);
        std::vector<bench::Query> result;
        for (uint64_t i = 0; i < queries; ++i) {
            bench::Query q;
            q.op = op(rng) ? bench::QueryOp::And : bench::QueryOp::Or;
            const size_t n = width(rng);
            for (size_t j = 0; j < n; ++j) {
                std::uniform_int_distribution<size_t> pick(j, pool.size() - 1);
                std::swap(pool[j], pool[pick(rng)]);
                q.ids.push_back(pool[j]);
            }
            result.push_back(std::move(q));
        }
        bench::write_queries(out, result);
        std::cout << "wrote " << queries << " queries to " << out << '\n';
    } catch (const std::exception &e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
