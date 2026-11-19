#include "tools/run_options.hpp"

#include <charconv>
#include <format>
#include <stdexcept>
#include <string>
#include <system_error>

#include "common/config.hpp"
#include "common/log.hpp"
#include "common/runtime_dirs.hpp"

namespace bb::tools {
namespace {

constexpr unsigned max_timeout_seconds = 86400;

unsigned parse_timeout(std::string_view value) {
    unsigned seconds = 0;
    const char* end = value.data() + value.size();
    const auto [ptr, ec] = std::from_chars(value.data(), end, seconds);
    if (ec != std::errc{} || ptr != end || seconds == 0 || seconds > max_timeout_seconds) {
        throw std::invalid_argument(
            std::format("--timeout: \"{}\" is not a number of seconds from 1 to {}", value,
                        max_timeout_seconds));
    }
    return seconds;
}

using common::max_rcas_attenuation;
using common::max_window_side;
using common::min_window_height;
using common::min_window_width;

// One side of --window-size.
bool parse_side(std::string_view text, unsigned min, unsigned& side) {
    const char* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, side);
    return ec == std::errc{} && ptr == end && side >= min && side <= max_window_side;
}

void parse_window_size(std::string_view value, RunOptions& options) {
    const std::size_t x = value.find('x');
    if (x == std::string_view::npos ||
        !parse_side(value.substr(0, x), min_window_width, options.window_width) ||
        !parse_side(value.substr(x + 1), min_window_height, options.window_height)) {
        throw std::invalid_argument(std::format(
            "--window-size: \"{}\" is not WIDTHxHEIGHT from {}x{} to {}x{}", value,
            min_window_width, min_window_height, max_window_side, max_window_side));
    }
}

void parse_resolution(std::string_view value, RunOptions& options) {
    const std::size_t x = value.find('x');
    unsigned width = 0;
    unsigned height = 0;
    const auto side = [](std::string_view text, unsigned& out) {
        const char* end = text.data() + text.size();
        const auto [ptr, ec] = std::from_chars(text.data(), end, out);
        return ec == std::errc{} && ptr == end;
    };
    if (x == std::string_view::npos || !side(value.substr(0, x), width) ||
        !side(value.substr(x + 1), height) || width < default_resolution_width ||
        width > max_resolution_width || width % resolution_width_step != 0 ||
        static_cast<unsigned long long>(width) * 9 != static_cast<unsigned long long>(height) * 16) {
        throw std::invalid_argument(std::format(
            "--resolution: \"{}\" is not a 16:9 WIDTHxHEIGHT from {}x{} to {}x{} with a width "
            "that is a multiple of {}",
            value, default_resolution_width, default_resolution_height, max_resolution_width,
            max_resolution_height, resolution_width_step));
    }
    options.resolution_width = width;
    options.resolution_height = height;
}

float parse_rcas_attenuation(std::string_view value) {
    float stops = 0;
    const char* end = value.data() + value.size();
    const auto [ptr, ec] = std::from_chars(value.data(), end, stops);
    // The negated test also refuses NaN.
    if (ec != std::errc{} || ptr != end || !(stops >= 0.0f && stops <= max_rcas_attenuation)) {
        throw std::invalid_argument(std::format(
            "--rcas-attenuation: \"{}\" is not a number from 0 to {}", value,
            max_rcas_attenuation));
    }
    return stops;
}

float parse_load_hold(std::string_view value) {
    float seconds = 0;
    const char* end = value.data() + value.size();
    const auto [ptr, ec] = std::from_chars(value.data(), end, seconds);
    // The negated test also refuses NaN.
    if (ec != std::errc{} || ptr != end || !(seconds >= 0.0f && seconds <= max_load_hold)) {
        throw std::invalid_argument(std::format(
            "--load-hold: \"{}\" is not a number of seconds from 0 to {}", value, max_load_hold));
    }
    return seconds;
}

}  // namespace

RunOptions options_from_config(const common::Config& config) {
    RunOptions options;
    options.game_dir = config.game_dir;
    if (config.profile) {
        options.profile = *config.profile;
    }
    if (config.window_size) {
        options.window_width = config.window_size->first;
        options.window_height = config.window_size->second;
    }
    options.fullscreen = config.fullscreen.value_or(options.fullscreen);
    options.fsr = config.fsr.value_or(options.fsr);
    options.rcas = config.rcas.value_or(options.rcas);
    options.rcas_attenuation = config.rcas_attenuation.value_or(options.rcas_attenuation);
    options.frame_rate = config.frame_rate.value_or(options.frame_rate);
    return options;
}

std::optional<std::filesystem::path> path_option(std::span<const std::string_view> args,
                                                 std::string_view name) {
    std::optional<std::filesystem::path> path;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == name) {
            if (i + 1 >= args.size()) {
                throw std::invalid_argument(std::format("{} needs a value", name));
            }
            path = std::filesystem::path(args[++i]);
        }
    }
    return path;
}

std::optional<std::filesystem::path> config_option(std::span<const std::string_view> args) {
    return path_option(args, "--config");
}

std::vector<std::string> setup_ignored_arguments(std::span<const std::string_view> args) {
    std::vector<std::string> ignored;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--setup" || args[i] == "--config") {
            ++i;  // its value
        } else {
            ignored.emplace_back(args[i]);
        }
    }
    return ignored;
}

RunOptions parse_run_options(std::span<const std::string_view> args) {
    return parse_run_options(args, RunOptions{});
}

RunOptions parse_run_options(std::span<const std::string_view> args, RunOptions options) {
    bool rcas_given = false;  // --no-rcas or --rcas-attenuation
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        // The value of an option that takes one.
        const auto value = [&]() -> std::string_view {
            if (i + 1 >= args.size()) {
                throw std::invalid_argument(std::format("{} needs a value", arg));
            }
            return args[++i];
        };
        if (arg == "-h" || arg == "--help") {
            options.help = true;
        } else if (arg == "--config" || arg == "--setup") {
            value();  // read before parsing, by config_option and path_option
        } else if (arg == "--game-dir") {
            options.game_dir = std::filesystem::path(value());
        } else if (arg == "--profile") {
            const std::string_view name = value();
            if (!common::valid_profile_name(name)) {
                throw std::invalid_argument(std::format(
                    "--profile: \"{}\" is not 1 to 32 characters from [A-Za-z0-9_-]", name));
            }
            options.profile = name;
        } else if (arg == "--timeout") {
            options.timeout_seconds = parse_timeout(value());
        } else if (arg == "--stop-at-main-loop") {
            options.stop_at_main_loop = true;
        } else if (arg == "--log") {
            const std::string_view spec = value();
            try {
                (void)common::LogFilter::parse(spec);
            } catch (const std::invalid_argument& e) {
                throw std::invalid_argument(std::format("--log: {}", e.what()));
            }
            options.log_spec = spec;
        } else if (arg == "--no-log-file") {
            options.log_file = false;
        } else if (arg == "--headless") {
            options.headless = true;
        } else if (arg == "--device") {
            options.device = value();
        } else if (arg == "--validation") {
            options.validation = true;
        } else if (arg == "--validation-sync") {
            options.validation_sync = true;
        } else if (arg == "--validation-gpu") {
            options.validation_gpu = true;
        } else if (arg == "--no-pipeline-cache") {
            options.pipeline_cache = false;
        } else if (arg == "--sync-pipelines") {
            options.async_pipelines = false;
        } else if (arg == "--sync-recording") {
            options.threaded_recording = false;
        } else if (arg == "--pad-input") {
            options.pad_input = std::filesystem::path(value());
        } else if (arg == "--frame-rate") {
            const std::string_view fps = value();
            unsigned rate = 0;
            for (const unsigned r : common::frame_rates) {
                rate = fps == std::to_string(r) ? r : rate;
            }
            if (rate == 0) {
                throw std::invalid_argument(
                    std::format("--frame-rate: \"{}\" is not 30, 60, 120 or 144", fps));
            }
            options.frame_rate = rate;
        } else if (arg == "--load-hold") {
            options.load_hold = parse_load_hold(value());
        } else if (arg == "--window-size") {
            parse_window_size(value(), options);
        } else if (arg == "--resolution") {
            parse_resolution(value(), options);
        } else if (arg == "--fullscreen") {
            options.fullscreen = true;
        } else if (arg == "--fsr") {
            options.fsr = true;
        } else if (arg == "--no-rcas") {
            options.rcas = false;
            rcas_given = true;
        } else if (arg == "--rcas-attenuation") {
            options.rcas_attenuation = parse_rcas_attenuation(value());
            rcas_given = true;
        } else if (arg == "--present-mode") {
            const std::string_view mode = value();
            if (mode == "fifo") {
                options.present_mode = PresentMode::fifo;
            } else if (mode == "mailbox") {
                options.present_mode = PresentMode::mailbox;
            } else if (mode == "immediate") {
                options.present_mode = PresentMode::immediate;
            } else {
                throw std::invalid_argument(
                    "--present-mode: expected fifo, mailbox or immediate");
            }
        } else {
            throw std::invalid_argument(std::format("unknown argument \"{}\"", arg));
        }
    }
    if ((options.validation_sync || options.validation_gpu) && !options.validation) {
        throw std::invalid_argument("--validation-sync and --validation-gpu need --validation");
    }
    if (rcas_given && !options.fsr) {
        // RCAS sharpens FSR's output and doesn't run without it.
        throw std::invalid_argument("--no-rcas and --rcas-attenuation need --fsr");
    }
    return options;
}

EntryArgs entry_args() {
    return {1, {eboot_guest_path, nullptr}};
}

}  // namespace bb::tools
