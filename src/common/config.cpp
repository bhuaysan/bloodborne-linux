#include "common/config.hpp"

#include <unistd.h>

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>

#include "common/runtime_dirs.hpp"

namespace bb::common {
namespace {

namespace fs = std::filesystem;

std::string_view trim(std::string_view s) {
    constexpr std::string_view space = " \t\r";
    const std::size_t first = s.find_first_not_of(space);
    if (first == std::string_view::npos) {
        return {};
    }
    return s.substr(first, s.find_last_not_of(space) + 1 - first);
}

// One value as written: a quoted string (already unescaped) or a bare word.
struct Value {
    bool quoted = false;
    std::string text;
};

class LineParser {
public:
    LineParser(std::string_view source, std::size_t line) : source_(source), line_(line) {}

    [[noreturn]] void fail(const std::string& what) const {
        throw ConfigError(std::format("{}:{}: {}", source_, line_, what));
    }

    Value value(std::string_view raw, std::string_view key) const {
        if (raw.empty()) {
            fail(std::format("{}: no value", key));
        }
        Value v;
        if (raw.front() != '"') {
            const std::size_t hash = raw.find('#');
            v.text = std::string(trim(raw.substr(0, hash)));
            return v;
        }
        v.quoted = true;
        std::size_t i = 1;
        for (; i < raw.size() && raw[i] != '"'; ++i) {
            if (raw[i] != '\\') {
                v.text += raw[i];
                continue;
            }
            if (++i == raw.size()) {
                break;
            }
            switch (raw[i]) {
            case '\\': v.text += '\\'; break;
            case '"': v.text += '"'; break;
            case 'n': v.text += '\n'; break;
            case 't': v.text += '\t'; break;
            default: fail(std::format("{}: unknown escape \\{}", key, raw[i]));
            }
        }
        if (i >= raw.size()) {
            fail(std::format("{}: the string is not closed", key));
        }
        const std::string_view rest = trim(raw.substr(i + 1));
        if (!rest.empty() && rest.front() != '#') {
            fail(std::format("{}: unexpected text after the string", key));
        }
        return v;
    }

private:
    std::string_view source_;
    std::size_t line_;
};

bool whole_number(std::string_view text, unsigned& out) {
    const char* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, out);
    return ec == std::errc{} && ptr == end;
}

}  // namespace

ParsedConfig parse_config(std::string_view text, std::string_view source) {
    ParsedConfig result;
    Config& c = result.config;
    std::map<std::string, std::size_t> seen;
    std::size_t line_no = 0;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        std::size_t eol = text.find('\n', pos);
        if (eol == std::string_view::npos) {
            eol = text.size();
        }
        const std::string_view line = trim(text.substr(pos, eol - pos));
        pos = eol + 1;
        ++line_no;
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const LineParser p(source, line_no);
        if (line.front() == '[') {
            p.fail("tables are not supported, write plain key = value lines");
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            p.fail(std::format("\"{}\" is not key = value", line));
        }
        const std::string key(trim(line.substr(0, eq)));
        if (key.empty()) {
            p.fail("a line has no key");
        }
        if (!seen.emplace(key, line_no).second) {
            p.fail(std::format("{} is set twice (first on line {})", key, seen[key]));
        }
        const Value v = p.value(trim(line.substr(eq + 1)), key);
        const auto want_bool = [&]() {
            if (v.quoted || (v.text != "true" && v.text != "false")) {
                p.fail(std::format("{}: \"{}\" is not true or false", key, v.text));
            }
            return v.text == "true";
        };
        const auto want_string = [&]() -> const std::string& {
            if (!v.quoted) {
                p.fail(std::format("{}: the value must be in double quotes", key));
            }
            return v.text;
        };
        // A decimal number from 0 to `max`.
        const auto want_number = [&](float max) {
            float number = 0;
            const char* end = v.text.data() + v.text.size();
            const auto [ptr, ec] = std::from_chars(v.text.data(), end, number);
            // The negated test also refuses NaN.
            if (v.quoted || ec != std::errc{} || ptr != end || !(number >= 0.0f && number <= max)) {
                p.fail(std::format("{}: \"{}\" is not a number from 0 to {}", key, v.text, max));
            }
            return number;
        };
        if (key == "game_dir") {
            std::string dir = want_string();
            const char* home = std::getenv("HOME");
            if (home && *home && (dir == "~" || dir.starts_with("~/"))) {
                dir = home + dir.substr(1);
            }
            if (dir.empty() || dir.front() != '/') {
                p.fail(std::format("game_dir: \"{}\" is not an absolute path", v.text));
            }
            c.game_dir = fs::path(dir);
        } else if (key == "profile") {
            if (!valid_profile_name(want_string())) {
                p.fail(std::format("profile: \"{}\" is not 1 to 32 characters from [A-Za-z0-9_-]",
                                   v.text));
            }
            c.profile = v.text;
        } else if (key == "window_size") {
            const std::string& s = want_string();
            const std::size_t x = s.find('x');
            unsigned w = 0;
            unsigned h = 0;
            if (x == std::string::npos || !whole_number(std::string_view(s).substr(0, x), w) ||
                !whole_number(std::string_view(s).substr(x + 1), h) || w < min_window_width ||
                h < min_window_height || w > max_window_side || h > max_window_side) {
                p.fail(std::format("window_size: \"{}\" is not WIDTHxHEIGHT from {}x{} to {}x{}", s,
                                   min_window_width, min_window_height, max_window_side,
                                   max_window_side));
            }
            c.window_size = std::pair{w, h};
        } else if (key == "fullscreen") {
            c.fullscreen = want_bool();
        } else if (key == "fsr") {
            c.fsr = want_bool();
        } else if (key == "rcas") {
            c.rcas = want_bool();
        } else if (key == "rcas_attenuation") {
            c.rcas_attenuation = want_number(max_rcas_attenuation);
        } else if (key == "frame_rate") {
            unsigned fps = 0;
            if (v.quoted || !whole_number(v.text, fps) || !valid_frame_rate(fps)) {
                p.fail(std::format("frame_rate: \"{}\" is not 30, 60, 120 or 144", v.text));
            }
            c.frame_rate = fps;
        } else if (key == "anisotropy") {
            unsigned level = 0;
            if (v.quoted || !whole_number(v.text, level) || !valid_anisotropy(level)) {
                p.fail(std::format("anisotropy: \"{}\" is not 1, 2, 4, 8 or 16", v.text));
            }
            c.anisotropy = level;
        } else if (key == "motion_blur") {
            c.motion_blur = want_bool();
        } else if (key == "motion_blur_strength") {
            c.motion_blur_strength = want_number(max_post_effect_strength);
        } else if (key == "chromatic_aberration") {
            c.chromatic_aberration = want_bool();
        } else if (key == "chromatic_aberration_strength") {
            c.chromatic_aberration_strength = want_number(max_post_effect_strength);
        } else {
            result.unknown_keys.push_back(key);
        }
    }
    return result;
}

ParsedConfig read_config(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw ConfigError(std::format("{}: cannot read the config file", path.string()));
    }
    std::ostringstream text;
    text << in.rdbuf();
    return parse_config(text.str(), path.string());
}

namespace {

std::string quote_string(std::string_view s) {
    std::string out = "\"";
    for (const char ch : s) {
        switch (ch) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        default: out += ch;
        }
    }
    return out + '"';
}

}  // namespace

std::string format_config(const Config& c) {
    std::string out;
    const auto flag = [&](const char* key, const std::optional<bool>& v) {
        if (v) {
            out += std::format("{} = {}\n", key, *v ? "true" : "false");
        }
    };
    if (c.game_dir) {
        out += std::format("game_dir = {}\n", quote_string(c.game_dir->string()));
    }
    if (c.profile) {
        out += std::format("profile = {}\n", quote_string(*c.profile));
    }
    if (c.window_size) {
        out += std::format("window_size = \"{}x{}\"\n", c.window_size->first, c.window_size->second);
    }
    flag("fullscreen", c.fullscreen);
    flag("fsr", c.fsr);
    flag("rcas", c.rcas);
    if (c.rcas_attenuation) {
        out += std::format("rcas_attenuation = {}\n", *c.rcas_attenuation);
    }
    if (c.frame_rate) {
        out += std::format("frame_rate = {}\n", *c.frame_rate);
    }
    if (c.anisotropy) {
        out += std::format("anisotropy = {}\n", *c.anisotropy);
    }
    flag("motion_blur", c.motion_blur);
    if (c.motion_blur_strength) {
        out += std::format("motion_blur_strength = {}\n", *c.motion_blur_strength);
    }
    flag("chromatic_aberration", c.chromatic_aberration);
    if (c.chromatic_aberration_strength) {
        out += std::format("chromatic_aberration_strength = {}\n",
                           *c.chromatic_aberration_strength);
    }
    return out;
}

void write_config(const fs::path& path, const Config& config) {
    const auto fail = [&](const std::string& what) {
        throw ConfigError(std::format("{}: cannot write the config file: {}", path.string(), what));
    };
    std::error_code ec;
    const fs::path dir = path.parent_path();
    if (!dir.empty()) {
        fs::create_directories(dir, ec);
        if (ec) {
            fail(ec.message());
        }
    }
    const fs::path temp = path.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << format_config(config);
        out.flush();
        if (!out) {
            fs::remove(temp, ec);
            fail("the file can't be written");
        }
    }
    fs::rename(temp, path, ec);
    if (ec) {
        fs::remove(temp, ec);
        fail("the file can't be replaced");
    }
}

void update_config(const fs::path& path, const std::function<void(Config&)>& change) {
    Config config;
    if (std::error_code ec; fs::exists(path, ec)) {
        config = read_config(path).config;
    }
    change(config);
    write_config(path, config);
}

std::optional<fs::path> default_config_path() {
    try {
        return RuntimeDirs::from_environment(default_profile, {}).config() / "config.toml";
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

}  // namespace bb::common
