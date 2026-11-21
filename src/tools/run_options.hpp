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
#include <vector>

#include "common/config.hpp"
#include "common/runtime_dirs.hpp"

namespace bb::tools {

inline constexpr std::string_view run_usage =
    "usage: bb-run [--config FILE] [--setup DIR] [--game-dir DIR] [--profile NAME] [--timeout SECONDS] [--stop-at-main-loop]\n"
    "              [--log SPEC] [--no-log-file] [--headless] [--device NAME] [--display NAME]\n"
    "              [--validation] [--validation-sync] [--validation-gpu]\n"
    "              [--present-mode fifo|mailbox|immediate] [--no-pipeline-cache]\n"
    "              [--sync-pipelines] [--sync-recording] [--pad-input PATH]\n"
    "              [--frame-rate 30|60|120|144] [--load-hold SECONDS]\n"
    "              [--window-size WIDTHxHEIGHT] [--fullscreen]\n"
    "              [--fsr] [--no-rcas]\n"
    "              [--rcas-attenuation STOPS] [--resolution WIDTHxHEIGHT]\n"
    "              [--anisotropy game|1|2|4|8|16]\n"
    "              [--no-motion-blur] [--motion-blur-strength S]\n"
    "              [--no-chromatic-aberration] [--chromatic-aberration-strength S]\n"
    "              [--fov DEGREES]\n"
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
    "  --display NAME       open the window centred on the display whose name contains NAME,\n"
    "                       ignoring case, and cover it with --fullscreen (default: the primary\n"
    "                       display); a warning and the primary display when none matches\n"
    "  --validation         load the Vulkan validation layer (core checks)\n"
    "  --validation-sync    with --validation: synchronization validation\n"
    "  --validation-gpu     with --validation: GPU-assisted validation\n"
    "  --present-mode MODE  the swapchain's present mode, falling back to fifo when the device\n"
    "                       lacks it (default: mailbox)\n"
    "  --no-pipeline-cache  neither read nor write the shader and pipeline cache on disk (below\n"
    "                       user/cache in the game's folder); every shader is translated and\n"
    "                       every pipeline built again, for comparison runs\n"
    "  --sync-pipelines     create every new pipeline on the GPU thread, so no draw is skipped\n"
    "                       while its pipeline is created on a worker thread (the default)\n"
    "  --sync-recording     record and submit Vulkan commands on the GPU thread instead of a\n"
    "                       recording thread of their own (the default)\n"
    "  --pad-input PATH     read pad states from the file or FIFO PATH and give them to the game\n"
    "                       through a gamepad that exists only in this process (docs/test-rig.md);\n"
    "                       needs a window; other controllers are then ignored\n"
    "  --frame-rate FPS     the game's target frame rate, 30 (the game's own, default), 60, 120 or\n"
    "                       144; the simulation steps 1/FPS seconds a frame\n"
    "                       (docs/design/frame-rate.md)\n"
    "  --load-hold SECONDS  the least time the loading page of a death's reload stays up, 0 to 12\n"
    "                       (default 12, the game's); the page still waits for the area to be\n"
    "                       ready\n"
    "  --window-size WxH    the window's size, from 640x360 to 16384x16384 (default 1280x720)\n"
    "  --fullscreen         a borderless window covering its display, at the display's size\n"
    "  --fsr                upscale the game's frame with AMD FSR 1 when the window is larger\n"
    "                       than the frame (the game renders at --resolution); without it the\n"
    "                       frame is scaled bilinearly\n"
    "  --no-rcas            with --fsr: no RCAS sharpening after the upscale\n"
    "  --rcas-attenuation STOPS\n"
    "                       with --fsr: RCAS sharpness, 0 (sharpest) to 2 stops of attenuation\n"
    "                       (default 0.25)\n"
    "  --resolution WxH     the size the game renders at: 16:9, the width a multiple of 64, from\n"
    "                       1920x1080 to 3840x2160 (default 1920x1080), or the wide 2560x1080,\n"
    "                       3440x1440 or 5120x1440 (more to the sides, the HUD at 16:9); the\n"
    "                       window shows it scaled to the window's size\n"
    "  --anisotropy N       the anisotropic filtering of textures with linear filtering and\n"
    "                       mipmaps, 1 (off), 2, 4, 8 or 16 (default: game, the game's own);\n"
    "                       sharpens ground seen at a grazing angle\n"
    "  --no-motion-blur     switch the game's motion blur off\n"
    "  --motion-blur-strength S\n"
    "                       scale the motion blur by S, 0 to 1 (default 1, the game's)\n"
    "  --no-chromatic-aberration\n"
    "                       switch the game's chromatic aberration (colour fringes towards the\n"
    "                       screen edges) off\n"
    "  --chromatic-aberration-strength S\n"
    "                       scale the chromatic aberration by S, 0 to 1 (default 1, the game's)\n"
    "  --fov DEGREES        the vertical field of view of the player's camera, 30 to 65, in place\n"
    "                       of the game's 43 (about 70 horizontal at 16:9); the game's other\n"
    "                       angles for it move by the same amount, cutscenes keep their own\n";

// The exit status of a run that hit --timeout, as timeout(1) has it.
inline constexpr int timeout_status = 124;

// --resolution: the engine's frame buffer size, common::valid_resolution. Above 1080p bb-run grows
// the engine's graphics heap with the size (set_graphics_heap_size).
using common::default_resolution_height;
using common::default_resolution_width;
using common::max_resolution_height;
using common::max_resolution_width;
using common::resolution_width_step;
using common::wide_resolutions;

// --load-hold: the game's minimum time of a death's loading page, and the most the option takes
// (bb::game::game_load_hold, #684). The config key load_hold has the same range.
using common::max_load_hold;

// --present-mode. Same order and meaning as Vulkan::PresentMode, which bb-run maps it to.
using common::PresentMode;

struct RunOptions {
    std::optional<std::filesystem::path> game_dir;  // --game-dir, else the environment
    std::string profile{common::default_profile};
    std::optional<unsigned> timeout_seconds;
    bool stop_at_main_loop = false;
    std::optional<std::string> log_spec;  // --log, checked with LogFilter::parse
    bool log_file = true;
    bool headless = false;  // --headless: no window and no presentation
    std::string device;     // --device: a device name part (resolve_gpu_id), empty for any
    // The device came from the config file: when it names no device (the card is gone), bb-run
    // warns and takes the default instead of ending the run. --device sets it to false.
    bool device_fallback = false;
    std::string display;  // --display: a display name part, empty for the primary display
    bool validation = false;       // --validation
    bool validation_sync = false;  // --validation-sync, only with --validation
    bool validation_gpu = false;   // --validation-gpu, only with --validation
    PresentMode present_mode = PresentMode::mailbox;  // --present-mode
    bool pipeline_cache = true;  // false with --no-pipeline-cache
    bool async_pipelines = true;  // false with --sync-pipelines
    bool threaded_recording = true;  // false with --sync-recording
    std::optional<std::filesystem::path> pad_input;  // --pad-input
    unsigned frame_rate = 30;  // --frame-rate: 30, 60, 120 or 144
    float load_hold = max_load_hold;  // --load-hold: 0 to 12 seconds
    unsigned window_width = 1280;  // --window-size
    unsigned window_height = 720;
    bool fullscreen = false;  // --fullscreen
    bool fsr = false;  // --fsr
    bool rcas = true;  // false with --no-rcas, only with --fsr
    float rcas_attenuation = 0.25f;  // --rcas-attenuation, only with --fsr
    unsigned resolution_width = default_resolution_width;  // --resolution
    unsigned resolution_height = default_resolution_height;
    unsigned anisotropy = 0;  // --anisotropy: 0 (game) or valid_anisotropy
    bool motion_blur = true;  // false with --no-motion-blur
    float motion_blur_strength = 1.0f;  // --motion-blur-strength: 0 to 1
    bool chromatic_aberration = true;  // false with --no-chromatic-aberration
    float chromatic_aberration_strength = 1.0f;  // --chromatic-aberration-strength: 0 to 1
    float fov = common::game_fov;  // --fov: common::min_fov to common::max_fov degrees
    bool help = false;  // -h or --help: print run_usage and exit 0
};

// Parses the arguments after argv[0]. Throws std::invalid_argument, naming the argument, for an
// unknown option, a missing value, a profile name that valid_profile_name refuses, a timeout that
// isn't a whole number from 1 to 86400, a log spec that LogFilter::parse refuses, a --present-mode
// that is none of fifo, mailbox and immediate, a --frame-rate other than 30, 60, 120 and 144, a
// --load-hold that isn't a number from 0 to 12, a --window-size that isn't WIDTHxHEIGHT in range, a
// --resolution that isn't one of the sizes above, an --rcas-attenuation that isn't a number from 0
// to 2, an --anisotropy other than game, 1, 2, 4, 8 and 16, --validation-sync or --validation-gpu
// without --validation, and --no-rcas or --rcas-attenuation without --fsr.
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

// The value of the last `name` (an option that takes a path) in `args`; same errors.
std::optional<std::filesystem::path> path_option(std::span<const std::string_view> args,
                                                 std::string_view name);

// The arguments of a --setup command line that it does not use: everything except --setup and
// --config with their values. bb-run warns about them instead of dropping them silently.
std::vector<std::string> setup_ignored_arguments(std::span<const std::string_view> args);

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
