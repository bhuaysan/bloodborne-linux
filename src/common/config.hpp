// bb-run's config file, $XDG_CONFIG_HOME/bloodborne-linux/config.toml (docs/design/launcher.md,
// "Config file"). One small reader and writer that bb-run and the launcher both link.
//
// The format is the flat subset of TOML that the settings need: `key = value` lines, strings in
// double quotes (escapes \\ \" \n \t), true and false, whole numbers, decimal numbers, and `#`
// comments. Tables and arrays are not supported. Every key is optional.
#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bb::common {

// Limits of the values, shared with bb-run's command line.
inline constexpr unsigned min_window_width = 640;  // the window's minimum size (sdl_window.cpp)
inline constexpr unsigned min_window_height = 360;
inline constexpr unsigned max_window_side = 16384;
inline constexpr float max_rcas_attenuation = 2.0f;
// The post effect strengths (game::PostEffects): a factor on the game's own value.
inline constexpr float max_post_effect_strength = 1.0f;

// The target frame rates bb-run takes (docs/design/frame-rate.md, "Above 60 FPS"). The sites the
// game paces in 30 FPS frames run on a 30 Hz clock of game time (game::game_tick), so a rate need
// not be a whole multiple of 30.
inline constexpr unsigned frame_rates[] = {30, 60, 120, 144};
// The anisotropy levels bb-run --anisotropy takes (#658).
inline constexpr unsigned anisotropy_levels[] = {1, 2, 4, 8, 16};
inline constexpr bool valid_anisotropy(unsigned level) {
    for (const unsigned l : anisotropy_levels) {
        if (level == l) {
            return true;
        }
    }
    return false;
}

inline constexpr bool valid_frame_rate(unsigned fps) {
    for (const unsigned rate : frame_rates) {
        if (fps == rate) {
            return true;
        }
    }
    return false;
}

struct Config {
    std::optional<std::filesystem::path> game_dir;  // game_dir: absolute, or "~/..." for $HOME
    std::optional<std::string> profile;             // profile: valid_profile_name
    std::optional<std::pair<unsigned, unsigned>> window_size;  // window_size = "WIDTHxHEIGHT"
    std::optional<bool> fullscreen;                 // fullscreen
    std::optional<bool> fsr;                        // fsr
    std::optional<bool> rcas;                       // rcas
    std::optional<float> rcas_attenuation;          // rcas_attenuation: 0 to 2 stops
    std::optional<unsigned> frame_rate;             // frame_rate: valid_frame_rate
    std::optional<unsigned> anisotropy;             // anisotropy: valid_anisotropy
    std::optional<bool> motion_blur;                // motion_blur
    std::optional<float> motion_blur_strength;      // motion_blur_strength: 0 to 1
    std::optional<bool> chromatic_aberration;       // chromatic_aberration
    std::optional<float> chromatic_aberration_strength;  // chromatic_aberration_strength: 0 to 1

    bool operator==(const Config&) const = default;
};

// A syntax error or a bad value. The message names the file, the line and the key.
class ConfigError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct ParsedConfig {
    Config config;
    std::vector<std::string> unknown_keys;  // keys that no setting has, ignored
};

// Parses `text`. `source` (the file's path) names it in error messages. Throws ConfigError for
// anything that isn't `key = value`, a repeated key, and a value that is the wrong type or out of
// range. Unknown keys are returned, not an error.
ParsedConfig parse_config(std::string_view text, std::string_view source);

// Reads and parses `path`. Throws ConfigError when the file can't be read or doesn't parse.
ParsedConfig read_config(const std::filesystem::path& path);

// The text of `config`, one line per setting that is set.
std::string format_config(const Config& config);

// Writes `config` to `path` through a temporary file in the same directory and a rename, creating
// the directory when it is missing. Throws ConfigError when that fails. Unknown keys and comments
// of an existing file are not kept.
void write_config(const std::filesystem::path& path, const Config& config);

// Reads `path` again (an empty config when it is missing), lets `change` set what it changes and
// writes the result, so keys another writer set since the caller last read the file stay. Throws
// ConfigError like read_config and write_config.
void update_config(const std::filesystem::path& path, const std::function<void(Config&)>& change);

// $XDG_CONFIG_HOME/bloodborne-linux/config.toml (HOME/.config when unset or relative). Nothing
// when neither is usable.
std::optional<std::filesystem::path> default_config_path();

}  // namespace bb::common
