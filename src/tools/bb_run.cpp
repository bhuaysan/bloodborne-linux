// bb-run: loads the game and runs it from its entry point (docs/design/runtime.md, "Tools";
// docs/design/m3-core-os.md, "Entry").
//
// The initial thread sets up the runtime, maps and links the load set, applies the registered
// hooks, opens the window and video_core's presenter (unless --headless; Liverpool alone then),
// starts video out and the GNM driver on them and starts the game's main thread. That thread runs
// the PRX initialisers and then the eboot's entry. The initial thread waits for it, or for
// --timeout, and handles the window's events; closing the window ends the run with status 0.
// The main-loop probe logs when the game reaches its main loop and, with --stop-at-main-loop,
// ends the run there, on the game's main thread. Every way out after the load goes through
// end_run, never std::exit: game threads may still run, and host atexit handlers, static
// destructors and LeakSanitizer must not run under them.

#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "common/config.hpp"
#include "common/env.hpp"
#include "common/game_folder.hpp"
#include "common/version.hpp"
#include "common/log.hpp"
#include "common/runtime_dirs.hpp"
#include "common/signal_stack.hpp"
#include "game/camera/aspect.hpp"
#include "game/camera/field_of_view.hpp"
#include "game/chr/ragdoll.hpp"
#include "game/frame/frame.hpp"
#include "game/frame/step_literals.hpp"
#include "game/gfx/frame_buffer.hpp"
#include "game/gfx/hud_stage.hpp"
#include "game/gfx/post_effects.hpp"
#include "game/image.hpp"
#include "game/load/load_hold.hpp"
#include "hle/kernel/paths.hpp"
#include "hle/kernel/thread.hpp"
#include "hle/kernel/time.hpp"
#include "hle/kernel/tls.hpp"
#include "hle/link.hpp"
#include "hle/prx_init.hpp"
#include "hle/system/save_data.hpp"
#include "hle/system/system.hpp"
#include "hle/video/video.hpp"
#include "hle/video/video_out.hpp"
#include "hook/hook.hpp"
#include "loader/load_set.hpp"
#include "tools/main_loop_probe.hpp"
#include "tools/run_options.hpp"
#ifdef BB_HAVE_VIDEO_CORE
#include "core/libraries/libs.h"
#include "core/libraries/pad_input.h"
#include "core/memory.h"
#include "hle/kernel/vmem.hpp"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "shadps4_host/fault_handler.hpp"
#endif
#ifdef BB_HAVE_WINDOW
#include "hle/video/video_core_presenter.hpp"
#include "sdl_window.h"
#endif

// The guest window lies in AddressSanitizer's shadow gap, which ASan keeps PROT_NONE without this
// option (docs/design/m3-core-os.md, Memory, "Guest window"). Unused without ASan.
extern "C" const char* __asan_default_options() {
    return "protect_shadow_gap=0";
}

namespace {

using bb::common::LogLevel;
namespace fs = std::filesystem;

constexpr std::string_view channel = "run";

// The game's main thread: 16 MiB (m3-core-os.md, "Entry"), plus the reserve that every game
// thread gets for HLE calls, the logger and the sanitizers ("Threads").
constexpr std::size_t main_stack_size =
    (std::size_t{16} << 20) + bb::hle::kernel::thread_stack_reserve;

#ifdef BB_HAVE_VIDEO_CORE
// video_core's guest memory, owned by start(). Created before the game starts, never destroyed:
// the run ends in end_run, which skips static destructors, as the GPU thread and the game do.
Core::MemoryManager* guest_memory = nullptr;
#endif

#ifdef BB_HAVE_WINDOW
void log_presented();
#endif

// Flushes stdio and the log and ends the process without host atexit handlers or static
// destructors, as the HLE _exit does.
[[noreturn]] void end_run(int status) {
#ifdef BB_HAVE_VIDEO_CORE
    // The driver's pipeline cache is saved here and by a timer, never by a destructor.
    Vulkan::PipelineCache::FlushActive();
    // How far video_core's backing covers what the game mapped (docs/design/renderer.md).
    if (guest_memory != nullptr) {
        const Core::MemoryManager::Coverage b = guest_memory->BackingCoverage();
        bb::common::log(LogLevel::info, channel,
                        "guest memory: {} regions, {} GPU-visible of which {} backed; {} pages, "
                        "{} backing entries",
                        b.all_regions, b.regions, b.covered, b.pages, b.table_pages);
        bb::common::log(LogLevel::info, channel,
                        "write tracking: {} faults claimed, {} system call writes announced",
                        bb::shadps4::claimed_faults(), bb::hle::kernel::vmem::host_writes());
    }
#endif
#ifdef BB_HAVE_WINDOW
    log_presented();
#endif
    std::fflush(nullptr);
    bb::common::log_flush();
    ::_exit(status);
}

#ifdef BB_HAVE_VIDEO_CORE
// SIGTERM (the rig's `systemctl stop`, timeout(1)) and SIGINT end the run through end_run, which
// saves the pipeline cache. Blocked in every thread, taken by one that waits for them; so it runs
// before any other thread starts.
void stop_on_signals() {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
    std::thread([set] {
        bb::common::set_thread_name("bb-run-signal");
        int sig = 0;
        if (sigwait(&set, &sig) == 0) {
            bb::common::log(LogLevel::info, channel, "signal {}, ending the run", sig);
            end_run(128 + sig);
        }
    }).detach();
}
#else
void stop_on_signals() {}
#endif

struct Game {
    bb::loader::LoadSet set;
    bb::loader::Image* eboot = nullptr;
};

// The main-loop probe and what to do when it fires. Set once before the game starts, never
// destroyed: game threads use it until the process ends.
struct MainLoop {
    bb::tools::MainLoopProbe probe;
    bool stop;  // --stop-at-main-loop
    std::chrono::steady_clock::time_point start;
};
MainLoop* main_loop = nullptr;

// The frame observer, on the game's main thread.
void observe_frame() {
    if (!main_loop->probe.count()) {
        return;
    }
    const std::chrono::duration<double> elapsed =
        std::chrono::steady_clock::now() - main_loop->start;
    bb::common::log(LogLevel::info, channel, "main loop reached after {:.1f} s", elapsed.count());
    if (main_loop->stop) {
        end_run(0);
    }
}

// The entry's exit handler: libc's exit calls it once main has returned (or the game called
// exit), so later frame pacer calls no longer count. PRX DT_FINI is not run in M3.
void exit_handler() {
    if (main_loop != nullptr) {
        main_loop->probe.main_returned();
    }
}

void* game_main(void* p) {
    const Game& game = *static_cast<const Game*>(p);
    bb::common::set_thread_name("game main");
    bb::common::ensure_signal_stack();  // for the fault handler, as on every game thread
    try {
        bb::hle::run_prx_init(game.set);
    } catch (const std::exception& e) {
        bb::common::log(LogLevel::error, channel, "PRX init: {}", e.what());
        end_run(1);
    }
    static const bb::tools::EntryArgs args = bb::tools::entry_args();
    auto* entry = game.eboot->fn<bb::tools::EbootEntry>(game.eboot->entry);
    bb::common::log(LogLevel::info, channel, "calling the entry {:#x}",
                    game.eboot->runtime(game.eboot->entry));
    entry(&args, &exit_handler);
    // The entry ends in libc's exit, which calls _exit; it doesn't return.
    bb::common::log(LogLevel::warn, channel, "the entry returned");
    end_run(0);
}

#ifdef BB_HAVE_WINDOW
// The window and video_core's presenter behind video out (docs/design/renderer.md, "Startup
// order"). Never destroyed: the present thread presents until the process ends.
struct Display {
    std::unique_ptr<Frontend::WindowSDL> window;
    std::unique_ptr<bb::hle::video::VideoCorePresenter> presenter;
};
Display* display = nullptr;

// Opens the window and creates the presenter on it. Without a display (neither WAYLAND_DISPLAY
// nor DISPLAY set) it logs that and leaves `display` null: the run then goes on headless. A window
// that fails to open on a display ends the run with status 1, a --device that names no device
// with status 2.
void open_display(const bb::tools::RunOptions& options) {
    if (std::getenv("WAYLAND_DISPLAY") == nullptr && std::getenv("DISPLAY") == nullptr) {
        bb::common::log(LogLevel::warn, channel,
                        "no window, running headless: neither WAYLAND_DISPLAY nor DISPLAY is set");
        if (options.pad_input) {
            bb::common::log(LogLevel::warn, channel, "--pad-input is not read without a window");
        }
        return;
    }
    std::string why;
    std::string display_note;
    auto d = std::make_unique<Display>();
    d->window = Frontend::WindowSDL::Open(
        "Bloodborne", static_cast<std::int32_t>(options.window_width),
        static_cast<std::int32_t>(options.window_height), options.fullscreen, options.display,
        display_note, why);
    if (!d->window) {
        bb::common::log(LogLevel::error, channel, "cannot open the window: {}", why);
        end_run(1);
    }
    if (!display_note.empty()) {
        bb::common::log(LogLevel::warn, channel, "--display: {}", display_note);
    }
    // The window passes the SDL gamepad events on to the imported pad, which the libraries
    // registered before.
    Frontend::WindowSDL::SetGamepadEventHandler(&Libraries::Pad::HandleGamepadEvent);
    Libraries::Pad::StartPolling();
    // After the window: the device order depends on its window system type.
    const Vulkan::ValidationOptions validation{options.validation, options.validation_sync,
                                               options.validation_gpu};
    std::optional<std::int32_t> gpu_id =
        bb::hle::video::resolve_gpu_id(options.device, d->window->GetWindowInfo().type, why,
                                       validation);
    if (!gpu_id && options.device_fallback) {
        // A card named in the config file that is gone: the game still starts.
        bb::common::log(LogLevel::warn, channel, "device in the config file: {}; using the default",
                        why);
        gpu_id = -1;
    }
    if (!gpu_id) {
        bb::common::log(LogLevel::error, channel, "--device: {}", why);
        end_run(2);
    }
    Storage::pipeline_cache_enabled = options.pipeline_cache;
    Vulkan::async_pipeline_builds = options.async_pipelines;
    Vulkan::threaded_recording = options.threaded_recording;
    Vulkan::RendererOptions renderer{*gpu_id, validation, Vulkan::PresentMode::Mailbox,
                                     options.fsr, options.rcas, options.rcas_attenuation,
                                     options.anisotropy};
    const char* const present_mode_name = bb::common::present_mode_name(options.present_mode);
    switch (options.present_mode) {
    case bb::tools::PresentMode::fifo: renderer.present_mode = Vulkan::PresentMode::Fifo; break;
    case bb::tools::PresentMode::mailbox: break;
    case bb::tools::PresentMode::immediate:
        renderer.present_mode = Vulkan::PresentMode::Immediate;
        break;
    }
    bb::common::log(LogLevel::info, channel, "renderer: device {}, present mode {}, validation {}",
                    *gpu_id, present_mode_name, options.validation ? "on" : "off");
    bb::common::log(LogLevel::info, channel,
                    "window {}x{}{} on display \"{}\", FSR {}, RCAS {} ({} stops)",
                    d->window->GetWidth(), d->window->GetHeight(),
                    options.fullscreen ? " fullscreen" : "", d->window->DisplayName(),
                    options.fsr ? "on" : "off",
                    options.fsr && options.rcas ? "on" : "off", options.rcas_attenuation);
    if (options.anisotropy != 0) {
        bb::common::log(LogLevel::info, channel, "anisotropy {}x on linear mipmapped samplers",
                        options.anisotropy);
    }
    d->presenter = std::make_unique<bb::hle::video::VideoCorePresenter>(d->window.get(),
                                                                         *guest_memory, renderer);
    display = d.release();
}

// Video out's presenter and the GNM driver's Liverpool: the display's, or without one Liverpool
// alone (docs/design/renderer.md, "Startup order"). Never destroyed, like the display.
bb::hle::video::VideoCorePresenter* video_core() {
    if (display != nullptr) {
        return display->presenter.get();
    }
    static auto* const headless = new bb::hle::video::VideoCorePresenter(nullptr, *guest_memory);
    return headless;
}

// Handles the window's events for up to `timeout_ms`; ends the run once it is closed.
void pump_display(std::int32_t timeout_ms) {
    if (!display->window->PumpEvents(timeout_ms)) {
        bb::common::log(LogLevel::info, channel, "window closed");
        end_run(0);
    }
}

void log_presented() {
    if (display != nullptr) {
        bb::common::log(LogLevel::info, channel, "{} game frames presented",
                        display->presenter->game_frames());
    }
    bb::common::log(LogLevel::info, channel, "{} draws, {} dispatches",
                    bb::hle::video::VideoCorePresenter::draws(),
                    bb::hle::video::VideoCorePresenter::dispatches());
}
#endif

// Starts game_main on a thread of its own and waits for it, at most `timeout_seconds`, handling
// the window's events meanwhile.
[[noreturn]] void run_game(Game& game, std::optional<unsigned> timeout_seconds) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, main_stack_size);
    pthread_t thread;
    if (const int err = pthread_create(&thread, &attr, &game_main, &game); err != 0) {
        bb::common::log(LogLevel::error, channel, "starting the game's main thread: errno {}",
                        err);
        end_run(1);
    }
    pthread_attr_destroy(&attr);
#ifdef BB_HAVE_WINDOW
    const bool windowed = display != nullptr;
#else
    const bool windowed = false;
#endif
    if (!timeout_seconds && !windowed) {
        pthread_join(thread, nullptr);
        end_run(0);  // not reached: game_main ends the process
    }
    timespec deadline{};
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += timeout_seconds.value_or(0);
    while (true) {
        timespec until = deadline;
#ifdef BB_HAVE_WINDOW
        if (windowed) {
            pump_display(16);  // waits for events up to 16 ms, then checks the game
            clock_gettime(CLOCK_MONOTONIC, &until);
        }
#endif
        const bool past_deadline = timeout_seconds && (until.tv_sec > deadline.tv_sec ||
                                                       (until.tv_sec == deadline.tv_sec &&
                                                        until.tv_nsec >= deadline.tv_nsec));
        if (past_deadline) {
            until = deadline;
        }
        const int err = pthread_clockjoin_np(thread, nullptr, CLOCK_MONOTONIC, &until);
        if (err == ETIMEDOUT && (!windowed || past_deadline)) {
            break;
        }
        if (err == ETIMEDOUT || err == EINTR) {
            continue;
        }
        end_run(0);  // not reached: game_main ends the process
    }
    bb::common::log(LogLevel::error, channel, "timeout: the game still runs after {} s",
                    *timeout_seconds);
    end_run(bb::tools::timeout_status);
}

// The config file that was read and its keys that no setting has, logged once the log is set up.
std::optional<fs::path> config_used;
std::vector<std::string> config_unknown_keys;

// Everything up to the start of the game's main thread. Returns the exit status of a run that
// ends before it; throws for errors that end it with status 1.
int start(const bb::tools::RunOptions& options) {
    const auto start_time = std::chrono::steady_clock::now();
    if (options.log_spec) {
        bb::common::set_log_filter(bb::common::LogFilter::parse(*options.log_spec));
    } else {
        bb::common::set_log_filter_from_environment();  // before link: the call trace reads it
    }
    bb::common::log(LogLevel::info, channel, "bloodborne-linux {}", bb::common::build_version());
    std::optional<fs::path> game_dir = options.game_dir;
    if (!game_dir) {
        game_dir = bb::common::game_dir("local.env");
    }
    if (!game_dir) {
        std::cerr << "bb-run: set game_dir in the config file, set BB_GAME_DIR (environment or ./local.env) or pass --game-dir\n";
        return 2;
    }
    game_dir = fs::absolute(*game_dir);
    const bb::common::RuntimeDirs dirs =
        bb::common::RuntimeDirs::from_environment(options.profile, {*game_dir});
    // Before SDL and the Vulkan loader. The signal thread is running, but it only waits in sigwait
    // and never reads the environment.
    bb::common::redirect_library_caches(dirs);
    if (options.log_file) {
        const fs::path log = bb::common::open_log_file(dirs);
        bb::common::log(LogLevel::info, channel, "log file {}", log.string());
    }
    bb::common::log(LogLevel::info, channel, "game {}, profile {}", game_dir->string(),
                    options.profile);
    if (config_used) {
        bb::common::log(LogLevel::info, channel, "config file {}", config_used->string());
        for (const std::string& key : config_unknown_keys) {
            bb::common::log(LogLevel::warn, channel, "config file: unknown key \"{}\" ignored",
                            key);
        }
    }

#ifdef BB_HAVE_VIDEO_CORE
    // video_core's guest memory follows the kernel's mappings from the first one on. Its write
    // tracking needs the fault handler (docs/design/renderer.md).
    guest_memory = new Core::MemoryManager;
    bb::shadps4::install_fault_handler();
    // The imported system libraries (docs/design/os-libraries.md), before the imports are linked.
    // A NID that src/hle registers too throws here.
    Core::Loader::SymbolsResolver symbols;
    if (options.pad_input && options.headless) {
        bb::common::log(LogLevel::warn, channel, "--pad-input is ignored: --headless has no window");
    } else if (options.pad_input) {
        // Before the libraries: the pad library gives slot 0 to the first pad SDL lists when it
        // registers, so the input's pad has to be there, and the other controllers ignored.
        std::string why;
        if (!Libraries::Pad::OpenPadInput(options.pad_input->string(), why)) {
            bb::common::log(LogLevel::error, channel, "--pad-input: {}", why);
            end_run(1);
        }
    }
    Libraries::InitHLELibs(&symbols, *guest_memory);
#endif
    // Never destroyed: the game runs in it until the process ends.
    auto* game = new Game{bb::loader::map_load_set(*game_dir)};
    game->eboot = &bb::hle::prepare(game->set, [&options](bb::loader::Image& eboot) {
        bb::hook::apply(eboot, bb::hook::replacements());
        bb::common::log(LogLevel::info, channel, "{} hooks applied",
                        bb::hook::replacements().size());
        // Before the entry point: the engine sets its heaps up at start, and the larger targets
        // of a larger frame buffer come out of GFX_GraphicsPrivate (docs/design/resolution.md).
        bb::game::set_graphics_heap_size(eboot, options.resolution_width,
                                         options.resolution_height);
        bb::common::log(LogLevel::info, channel, "GFX_GraphicsPrivate heap {} MiB",
                        bb::game::graphics_private_heap_size(options.resolution_width,
                                                             options.resolution_height) >> 20);
        // The HUD places its world markers in 1920x1080 stage coordinates (#713).
        bb::game::set_hud_stage_loads(eboot, options.resolution_width, options.resolution_height);
        // Black, not the game's grey, beside the 16:9 menus at a wider frame buffer (#711).
        bb::game::set_device_clear_color(eboot, options.resolution_width,
                                         options.resolution_height);
    });
    bb::game::set_image(game->eboot);
    // Before the entry point: system_properties_init and the graphics device read the size from
    // the globals (docs/design/resolution.md).
    bb::game::set_frame_buffer_size(options.resolution_width, options.resolution_height);
    bb::game::set_screen_aspect(options.resolution_width, options.resolution_height);
    bb::common::log(LogLevel::info, channel, "rendering at {}x{}", options.resolution_width,
                    options.resolution_height);
    bb::hle::kernel::check_eboot_tls();

    bb::hle::kernel::path_map().mount_game(*game_dir);
    bb::hle::system::set_save_data_root(bb::hle::system::save_data_root(dirs));
    bb::hle::system::set_user_name(options.profile);
    main_loop = new MainLoop{bb::tools::MainLoopProbe(), options.stop_at_main_loop, start_time};
    bb::game::set_frame_observer(&observe_frame);
    bb::game::set_frame_rate(options.frame_rate);
    static_assert(bb::tools::max_load_hold == bb::game::game_load_hold,
                  "--load-hold's range must end at the game's hold");
    bb::game::set_load_hold(options.load_hold);
    if (options.load_hold != bb::game::game_load_hold) {
        bb::common::log(LogLevel::info, channel, "loading page hold {} s (the game's: {} s)",
                        options.load_hold, bb::game::game_load_hold);
    }
    static_assert(bb::common::max_post_effect_strength == bb::game::max_post_effect_strength);
    bb::game::set_post_effects({options.motion_blur, options.motion_blur_strength,
                                options.chromatic_aberration,
                                options.chromatic_aberration_strength});
    if (bb::game::post_effects() != bb::game::PostEffects{}) {
        bb::common::log(LogLevel::info, channel,
                        "motion blur {} (strength {}), chromatic aberration {} (strength {})",
                        options.motion_blur ? "on" : "off", options.motion_blur_strength,
                        options.chromatic_aberration ? "on" : "off",
                        options.chromatic_aberration_strength);
    }
    static_assert(bb::common::min_fov == bb::game::min_fov &&
                  bb::common::max_fov == bb::game::max_fov);
    static_assert(bb::common::game_fov == bb::game::game_fov,
                  "--fov's default must be the game's angle");
    bb::game::set_field_of_view(options.fov);
    if (options.fov != bb::game::game_fov) {
        bb::common::log(LogLevel::info, channel, "field of view {} degrees (the game's: {})",
                        options.fov, bb::game::game_fov);
    }
    bb::hle::video::set_vblank_rate(options.frame_rate);
    bb::common::log(LogLevel::info, channel, "target frame rate {} FPS, vblank period {} ns",
                    options.frame_rate,
                    bb::hle::video::vblank_timing(options.frame_rate).period.count());
#ifdef BB_HAVE_WINDOW
    if (!options.headless) {
        open_display(options);
    }
    bb::hle::video::start_video_out(video_core());
    bb::hle::video::start_gnm(video_core());
#else
    bb::hle::video::start_video_out(nullptr);
    bb::hle::video::start_gnm(nullptr);
    if (options.pad_input) {
        bb::common::log(LogLevel::warn, channel, "--pad-input is ignored: built without a window");
    }
#endif
    run_game(*game, options.timeout_seconds);
}

}  // namespace

int main(int argc, char** argv) {
    // --version reads nothing: no config file, no game, no user folder.
    if (std::find(argv + 1, argv + argc, std::string_view("--version")) != argv + argc) {
        std::cout << "bb-run " << bb::common::build_version() << '\n';
        return 0;
    }
    bb::tools::RunOptions options;
    try {
        const std::vector<std::string_view> args(argv + 1, argv + argc);
        // The config file first: the command line wins over it.
        std::optional<fs::path> config_file = bb::tools::config_option(args);
        if (const auto setup = bb::tools::path_option(args, "--setup")) {
            // Checks the folder and writes the config file; it reads no settings from it.
            if (!config_file) {
                config_file = bb::common::default_config_path();
            }
            if (!config_file) {
                std::cerr << "bb-run: no config file location: the game's folder can't be found, or set BB_USER_DIR\n";
                return 1;
            }
            const bb::common::SetupResult r = bb::common::setup_game_folder(*setup, *config_file);
            if (!r.ok()) {
                std::cerr << "bb-run: " << r.message() << '\n';
                return 1;
            }
            std::string ignored;
            for (const std::string& arg : bb::tools::setup_ignored_arguments(args)) {
                ignored += (ignored.empty() ? "" : " ") + arg;
            }
            if (!ignored.empty()) {
                std::cerr << "bb-run: --setup ignores the rest of the command line: " << ignored << '\n';
            }
            std::cout << r.folder.message << "\nGame folder " << fs::canonical(*setup).string()
                      << " written to " << config_file->string() << '\n';
            return 0;
        }
        const bool explicit_config = config_file.has_value();
        if (!config_file) {
            config_file = bb::common::default_config_path();
        }
        bb::tools::RunOptions base;
        std::error_code exists_error;  // an unreadable directory counts as no file
        if (config_file && (explicit_config || fs::exists(*config_file, exists_error))) {
            const bb::common::ParsedConfig parsed = bb::common::read_config(*config_file);
            base = bb::tools::options_from_config(parsed.config);
            config_unknown_keys = parsed.unknown_keys;
            config_used = config_file;
        }
        options = bb::tools::parse_run_options(args, base);
    } catch (const bb::common::ConfigError& e) {
        // --help works with a broken config file.
        const std::vector<std::string_view> args(argv + 1, argv + argc);
        if (std::ranges::find(args, "--help") != args.end() ||
            std::ranges::find(args, "-h") != args.end()) {
            std::cout << bb::tools::run_usage;
            return 0;
        }
        std::cerr << "bb-run: " << e.what() << '\n';
        return 2;
    } catch (const std::invalid_argument& e) {
        std::cerr << "bb-run: " << e.what() << '\n' << bb::tools::run_usage;
        return 2;
    }
    if (options.help) {
        std::cout << bb::tools::run_usage;
        return 0;
    }
    bb::common::set_thread_name("bb-run");
    stop_on_signals();
    try {
        return start(options);
    } catch (const std::exception& e) {
        bb::common::log(LogLevel::error, channel, "{}", e.what());
        end_run(1);
    }
}
