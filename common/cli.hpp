#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace bench {
inline uint64_t parse_unsigned(const std::string &value, const std::string &name) {
    if (value.empty() ||
        !std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; }))
        throw std::runtime_error(name + " must be a nonnegative integer");
    try {
        return std::stoull(value);
    } catch (const std::exception &) {
        throw std::runtime_error(name + " is out of range");
    }
}
inline double parse_real(const std::string &value, const std::string &name) {
    size_t end = 0;
    try {
        const double result = std::stod(value, &end);
        if (end == value.size() && std::isfinite(result))
            return result;
    } catch (const std::exception &) {
    }
    throw std::runtime_error(name + " must be a finite number");
}
class Arguments {
  public:
    Arguments(int argc, char **argv, const std::vector<std::string> &names,
              const std::vector<std::string> &flags = {}) {
        for (int i = 1; i < argc; ++i) {
            const std::string token = argv[i];
            if (token == "--help" || token == "-h") {
                help = true;
                continue;
            }
            if (token.rfind("--", 0) != 0)
                throw std::runtime_error("expected --option=value: " + token);
            const size_t eq = token.find('=');
            const std::string name = token.substr(2, eq == std::string::npos ? eq : eq - 2);
            if (std::find(names.begin(), names.end(), name) == names.end())
                throw std::runtime_error("unknown option: --" + name);
            std::string value;
            if (eq != std::string::npos)
                value = token.substr(eq + 1);
            else if (std::find(flags.begin(), flags.end(), name) != flags.end())
                value = "1";
            else
                throw std::runtime_error("expected --" + name + "=value");
            if (!values_.emplace(name, value).second)
                throw std::runtime_error("duplicate option: --" + name);
        }
    }
    std::string get(const std::string &name, const std::string &fallback = "") const {
        auto it = values_.find(name);
        return it == values_.end() ? fallback : it->second;
    }
    bool has(const std::string &name) const { return values_.count(name) != 0; }
    uint64_t number(const std::string &name, uint64_t fallback) const {
        return has(name) ? parse_unsigned(get(name), "--" + name) : fallback;
    }
    bool boolean(const std::string &name, bool fallback = false) const {
        if (!has(name))
            return fallback;
        const auto value = get(name);
        if (value != "0" && value != "1")
            throw std::runtime_error("--" + name + " must be 0 or 1");
        return value == "1";
    }
    bool help = false;

  private:
    std::map<std::string, std::string> values_;
};
inline std::string trim(std::string s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return "";
    return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}
inline std::string csv_field(const std::string &value) {
    if (value.find_first_of(",\"\r\n") == std::string::npos)
        return value;
    std::string result = "\"";
    for (char c : value) {
        if (c == '\"')
            result += '\"';
        result += c;
    }
    return result + '\"';
}
} // namespace bench
