// The command line of bb-run and the argument block it hands to the eboot's entry
// (docs/design/m3-core-os.md, "Entry"; docs/design/runtime.md, "Tools").
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "common/config.hpp"
#include "common/runtime_dirs.hpp"

namespace bb::tools {

inline constexpr std::string_view run_usage =
    "usage: bb-run [--config FILE] [--game-dir DIR] [--profile NAME] [--timeout SECONDS] [--stop-at-main-loop]\n"
    "              [--log SPEC] [--no-log-file] [--headless] [--device NAME]\n"
    "              [--validation] [--validation-sync] [--validation-gpu]\n"
    "              [--present-mode fifo|mailbox|immediate] [--no-pipeline-cache]\n"
    "              [--sync-pipelines] [--pad-input PATH] [--frame-rate 30|60]\n"
    "              [--window-size WIDTHxHEIGHT] [--fullscreen] [--fsr] [--no-rcas]\n"
    "              [--rcas-attenuation STOPS]\n"
    "Loads eboot.bin and the bundled PRXs of the game in DIR and runs the game from its entry\n"
    "point. DIR defaults to $BB_GAME_DIR, else BB_GAME_DIR in ./local.env.\n"
    "  --profile NAME       save data and temporary files of profile NAME ([A-Za-z0-9_-], at\n"
    "                       most 32 characters; default: default)\n"
    "  --timeout SECONDS    end the run with status 124 when the game still runs after SECONDS\n"
    "  --stop-at-main-loop  exit with status 0 once the game reaches its main loop\n"
    "  --log SPEC           log filter, as BB_LOG: <channel>=<level> or <level>, comma-separated\n"
    "                       (replaces BB_LOG)\n"
    "  --no-log-file        log to stderr only, not to a file in the logs directory\n"
    "  --headless           open no window; flips complete without being presented (also when\n"
    "                       neither WAYLAND_DISPLAY nor DISPLAY is set)\n"
    "  --device NAME        the Vulkan device whose name contains NAME, ignoring case\n"
    "                       (default: video_core's choice, a discrete GPU first); status 2 when\n"
    "                       none matches\n"
    "  --validation         load the Vulkan validation layer (core checks)\n"
    "  --validation-sync    with --validation: synchronization validation\n"
    "  --validation-gpu     with --validation: GPU-assisted validation\n"
    "  --present-mode MODE  the swapchain's present mode, falling back to fifo when the device\n"
    "                       lacks it (default: mailbox)\n"
    "  --no-pipeline-cache  neither read nor write the shader and pipeline cache on disk (below\n"
    "                       $XDG_CACHE_HOME/bloodborne-linux); every shader is translated and\n"
    "                       every pipeline built again, for comparison runs\n"
    "  --sync-pipelines     create every new pipeline on the GPU thread, so no draw is skipped\n"
    "                       while its pipeline is created on a worker thread (the default)\n"
    "  --pad-input PATH     read pad states from the file or FIFO PATH and give them to the game\n"
    "                       through a gamepad that exists only in this process (docs/test-rig.md);\n"
    "                       needs a window; other controllers are then ignored\n"
    "  --frame-rate FPS     the game's target frame rate, 30 (the game's own, default) or 60; the\n"
    "                       simulation steps 1/FPS seconds a frame (docs/design/frame-rate.md)\n"
    "  --window-size WxH    the window's size, from 640x360 to 16384x16384 (default 1280x720)\n"
    "  --fullscreen         a borderless window covering its display, at the display's size\n"
    "  --fsr                upscale the game's frame with AMD FSR 1 when the window is larger\n"
    "                       than the frame (the game renders 1920x1080); without it the frame is\n"
    "                       scaled bilinearly\n"
    "  --no-rcas            with --fsr: no RCAS sharpening after the upscale\n"
    "  --rcas-attenuation STOPS\n"
    "                       with --fsr: RCAS sharpness, 0 (sharpest) to 2 stops of attenuation\n"
    "                       (default 0.25)\n";

// The exit status of a run that hit --timeout, as timeout(1) has it.
inline constexpr int timeout_status = 124;

// --present-mode. Same order and meaning as Vulkan::PresentMode, which bb-run maps it to.
enum class PresentMode { fifo, mailbox, immediate };

struct RunOptions {
    std::optional<std::filesystem::path> game_dir;  // --game-dir, else the environment
    std::string profile{common::default_profile};
    std::optional<unsigned> timeout_seconds;
    bool stop_at_main_loop = false;
    std::optional<std::string> log_spec;  // --log, checked with LogFilter::parse
    bool log_file = true;
    bool headless = false;  // --headless: no window and no presentation
    std::string device;     // --device: a device name part (resolve_gpu_id), empty for any
    bool validation = false;       // --validation
    bool validation_sync = false;  // --validation-sync, only with --validation
    bool validation_gpu = false;   // --validation-gpu, only with --validation
    PresentMode present_mode = PresentMode::mailbox;  // --present-mode
    bool pipeline_cache = true;  // false with --no-pipeline-cache
    bool async_pipelines = true;  // false with --sync-pipelines
    std::optional<std::filesystem::path> pad_input;  // --pad-input
    unsigned frame_rate = 30;  // --frame-rate: 30 or 60
    unsigned window_width = 1280;  // --window-size
    unsigned window_height = 720;
    bool fullscreen = false;  // --fullscreen
    bool fsr = false;  // --fsr
    bool rcas = true;  // false with --no-rcas, only with --fsr
    float rcas_attenuation = 0.25f;  // --rcas-attenuation, only with --fsr
    std::optional<std::filesystem::path> config_path;  // --config
    bool help = false;  // -h or --help: print run_usage and exit 0
};

// Parses the arguments after argv[0]. Throws std::invalid_argument, naming the argument, for an
// unknown option, a missing value, a profile name that valid_profile_name refuses, a timeout that
// isn't a whole number from 1 to 86400, a log spec that LogFilter::parse refuses, a --present-mode
// that is none of fifo, mailbox and immediate, a --frame-rate other than 30 and 60, a
// --window-size that isn't WIDTHxHEIGHT in range, an --rcas-attenuation that isn't a number from 0
// to 2, --validation-sync or --validation-gpu without --validation, and --no-rcas or
// --rcas-attenuation without --fsr.
RunOptions parse_run_options(std::span<const std::string_view> args);

// The same, starting from `base` (the options that the config file gave) instead of the defaults:
// the arguments win over it. --no-rcas and --rcas-attenuation need fsr to be on, from the base or
// from --fsr.
RunOptions parse_run_options(std::span<const std::string_view> args, RunOptions base);

// The defaults with the settings of `config` applied.
RunOptions options_from_config(const common::Config& config);

// The value of the last --config in `args`, so that the file can be read before the rest is parsed.
// Throws std::invalid_argument when --config has no value.
std::optional<std::filesystem::path> config_option(std::span<const std::string_view> args);

// The eboot's entry reads argc from +0 of its first argument and takes argv at +8 (link address
// 0xa4..0xb4), then calls main(argc, argv, nullptr).
struct EntryArgs {
    std::int32_t argc;
    const char* argv[2];  // argv[argc] is null, as in C
};
static_assert(offsetof(EntryArgs, argc) == 0);
static_assert(offsetof(EntryArgs, argv) == 8);

// The game's path of its own executable, argv[0].
inline constexpr const char* eboot_guest_path = "/app0/eboot.bin";

// argc 1 and argv {eboot_guest_path, nullptr}.
EntryArgs entry_args();

// The eboot's entry point: entry(args, exit_handler). The entry registers exit_handler with libc's
// atexit, so libc's exit calls it, once main has returned or the game calls exit.
using EbootEntry = void(const EntryArgs* args, void (*exit_handler)());

}  // namespace bb::tools
