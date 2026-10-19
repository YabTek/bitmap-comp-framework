#include "common/query.hpp"

#include "common/cli.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace bench {

std::vector<Query> read_queries(const std::string &path) {
    std::ifstream in(path);
    if (!in)
        throw std::runtime_error("cannot open " + path);
    std::vector<Query> queries;
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream iss(line);
        std::string op_str;
        std::string count_text;
        if (!(iss >> op_str >> count_text))
            throw std::runtime_error("bad query line: " + line);
        const uint64_t n = parse_unsigned(count_text, "query width");
        Query q;
        if (op_str == "AND")
            q.op = QueryOp::And;
        else if (op_str == "OR")
            q.op = QueryOp::Or;
        else
            throw std::runtime_error("unknown op: " + op_str);
        std::string token;
        while (iss >> token)
            q.ids.push_back(static_cast<size_t>(parse_unsigned(token, "bitmap index")));
        if (!n || q.ids.size() != n)
            throw std::runtime_error("query width/operand count mismatch: " + line);
        queries.push_back(std::move(q));
    }
    if (queries.empty())
        throw std::runtime_error("query file contains no queries");
    return queries;
}

void write_queries(const std::string &path, const std::vector<Query> &queries) {
    auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty())
        std::filesystem::create_directories(parent);
    std::ofstream out(path);
    if (!out)
        throw std::runtime_error("cannot write " + path);
    for (const auto &q : queries) {
        out << query_op_str(q.op) << " " << q.ids.size();
        for (size_t id : q.ids)
            out << " " << id;
        out << "\n";
    }
    out.close();
    if (!out)
        throw std::runtime_error("failed writing " + path);
}

} // namespace bench
