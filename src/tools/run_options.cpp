#include "tools/run_options.hpp"

#include <charconv>
#include <format>
#include <stdexcept>
#include <system_error>

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

}  // namespace

RunOptions parse_run_options(std::span<const std::string_view> args) {
    RunOptions options;
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
        } else {
            throw std::invalid_argument(std::format("unknown argument \"{}\"", arg));
        }
    }
    return options;
}

EntryArgs entry_args() {
    return {1, {eboot_guest_path, nullptr}};
}

}  // namespace bb::tools
