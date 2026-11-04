#include "common/env.hpp"

#include <cstdlib>
#include <fstream>

namespace bb::common {
namespace {

// Python's str.strip() also strips \v, \f and Unicode spaces. They don't occur in local.env.
std::string_view trim(std::string_view s) {
    constexpr std::string_view space = " \t\r";
    auto first = s.find_first_not_of(space);
    if (first == std::string_view::npos) {
        return {};
    }
    auto last = s.find_last_not_of(space);
    return s.substr(first, last - first + 1);
}

// Strips any leading and trailing quote characters, like Python's strip("\"'").
std::string_view unquote(std::string_view s) {
    constexpr std::string_view quotes = "\"'";
    auto first = s.find_first_not_of(quotes);
    if (first == std::string_view::npos) {
        return {};
    }
    auto last = s.find_last_not_of(quotes);
    return s.substr(first, last - first + 1);
}

}  // namespace

std::optional<std::string> read_env_file(const std::filesystem::path& file, std::string_view key) {
    std::ifstream in(file);
    std::optional<std::string> value;
    for (std::string line; std::getline(in, line);) {
        std::string_view rest = trim(line);
        auto eq = rest.find('=');
        if (eq == std::string_view::npos) {
            continue;
        }
        std::string_view name = rest.substr(0, eq);
        if (name.starts_with("export ")) {
            name.remove_prefix(7);
        }
        if (trim(name) == key) {
            value = std::string(unquote(trim(rest.substr(eq + 1))));
        }
    }
    if (value && value->empty()) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::string> env_or_file(std::string_view key, const std::filesystem::path& env_file) {
    const char* value = std::getenv(std::string(key).c_str());
    if (value && *value) {
        return std::string(value);
    }
    return read_env_file(env_file, key);
}

std::optional<std::filesystem::path> game_dir(const std::filesystem::path& env_file) {
    auto value = env_or_file("BB_GAME_DIR", env_file);
    if (!value) {
        return std::nullopt;
    }
    std::filesystem::path path(*value);
    const char* home = std::getenv("HOME");
    if (home && *home && (*value == "~" || value->starts_with("~/"))) {
        path = std::filesystem::path(home) / value->substr(value->size() > 1 ? 2 : 1);
    }
    std::error_code ec;
    auto resolved = std::filesystem::weakly_canonical(std::filesystem::absolute(path), ec);
    return ec ? std::filesystem::absolute(path) : resolved;
}

}  // namespace bb::common
