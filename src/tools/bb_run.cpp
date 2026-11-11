// bb-run: loads the game and runs it from its entry point (docs/design/runtime.md, "Tools";
// docs/design/m3-core-os.md, "Entry").
//
// The initial thread sets up the runtime, maps and links the load set, applies the registered
// hooks, opens the window and video_core's presenter (unless --headless), starts video out and
// starts the game's main thread. That thread runs
// the PRX initialisers and then the eboot's entry. The initial thread waits for it, or for
// --timeout, and handles the window's events; closing the window ends the run with status 0.
// The main-loop probe logs when the game reaches its main loop and, with --stop-at-main-loop,
// ends the run there, on the game's main thread. Every way out after the load goes through
// end_run, never std::exit: game threads may still run, and host atexit handlers, static
// destructors and LeakSanitizer must not run under them.

#include <pthread.h>
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
#include <vector>

#include "common/env.hpp"
#include "common/log.hpp"
#include "common/runtime_dirs.hpp"
#include "common/signal_stack.hpp"
#include "game/image.hpp"
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
#ifdef BB_HAVE_MIRROR
#include "gpu/vk/compute_pipeline.hpp"
#include "gpu/vk/device.hpp"
#include "gpu/vk/dispatcher.hpp"
#include "gpu/vk/draw_recorder.hpp"
#include "gpu/vk/image_cache.hpp"
#include "gpu/vk/pipeline_cache.hpp"
#include "gpu/vk/rendering.hpp"
#include "gpu/vk/memory_mirror.hpp"
#include "gpu/vk/scheduler.hpp"
#include "gpu/vk/texture_binder.hpp"
#include "gpu/vk/texture_cache.hpp"
#include "hle/kernel/dmem.hpp"
#include "hle/kernel/vmem.hpp"
#include "tools/mirror_observer.hpp"
#endif
#ifdef BB_HAVE_VIDEO_CORE
#include "core/memory.h"
#include "hle/kernel/vmem.hpp"
#include "shadps4_host/fault_handler.hpp"
#endif
#ifdef BB_HAVE_WINDOW
#include "core/emulator_settings.h"
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
// video_core's guest memory. Set once before the game starts, never destroyed.
Core::MemoryManager* guest_memory = nullptr;
#endif

#ifdef BB_HAVE_MIRROR
// The GPU memory mirror and what keeps it in step with direct memory. Set once before the game
// starts, never destroyed: game threads map and unmap until the process ends.
struct Mirror {
    std::unique_ptr<bb::vk::Instance> instance;  // only when no display has the device
    std::unique_ptr<bb::vk::Device> device;
    std::unique_ptr<bb::vk::MemoryMirror> mirror;
    std::unique_ptr<bb::tools::MirrorObserver> observer;
    // The dispatches of the game's compute and draw streams, recorded on the GPU thread.
    std::unique_ptr<bb::vk::Scheduler> scheduler;
    std::unique_ptr<bb::vk::ComputePipelineCache> pipelines;
    std::unique_ptr<bb::vk::Dispatcher> dispatcher;
    std::unique_ptr<bb::vk::ImageCache> images;
    std::unique_ptr<bb::vk::TargetRenderer> targets;
    std::unique_ptr<bb::vk::PipelineCache> draw_pipelines;
    std::unique_ptr<bb::vk::TextureCache> textures;
    std::unique_ptr<bb::vk::TextureBinder> binder;
    std::unique_ptr<bb::vk::DrawRecorder> recorder;
};
Mirror* mirror = nullptr;
#endif

#ifdef BB_HAVE_WINDOW
void log_presented();
#endif

// Flushes stdio and the log and ends the process without host atexit handlers or static
// destructors, as the HLE _exit does.
[[noreturn]] void end_run(int status) {
#ifdef BB_HAVE_VIDEO_CORE
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
#ifdef BB_HAVE_MIRROR
    if (mirror != nullptr && mirror->observer) {
        bb::common::log(LogLevel::info, channel, "mirror: {} binds, {} unbinds, {} releases",
                        mirror->observer->binds(), mirror->observer->unbinds(),
                        mirror->observer->releases());
    }
    if (mirror != nullptr && mirror->dispatcher) {
        const auto stats = mirror->dispatcher->stats();
        bb::common::log(LogLevel::info, channel,
                        "dispatches: {} recorded, {} untranslated, {} failed", stats.recorded,
                        stats.untranslated, stats.failed);
    }
    if (mirror != nullptr && mirror->recorder) {
        const auto stats = mirror->recorder->stats();
        bb::common::log(LogLevel::info, channel, "draws: {} recorded, {} unsupported, {} failed",
                        stats.recorded, stats.unsupported, stats.failed);
    }
#endif
#ifdef BB_HAVE_WINDOW
    log_presented();
#endif
    std::fflush(nullptr);
    bb::common::log_flush();
    ::_exit(status);
}

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

// The gettimeofday observer (libScePosix), on the calling game thread.
void observe_gettimeofday(std::uintptr_t return_address) {
    if (!main_loop->probe.count(return_address)) {
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
void open_display(const std::string& device_selector) {
    if (std::getenv("WAYLAND_DISPLAY") == nullptr && std::getenv("DISPLAY") == nullptr) {
        bb::common::log(LogLevel::warn, channel,
                        "no window, running headless: neither WAYLAND_DISPLAY nor DISPLAY is set");
        return;
    }
    std::string why;
    auto d = std::make_unique<Display>();
    d->window = Frontend::WindowSDL::Open("Bloodborne", 1280, 720, why);
    if (!d->window) {
        bb::common::log(LogLevel::error, channel, "cannot open the window: {}", why);
        end_run(1);
    }
    // After the window: the device order depends on its window system type.
    const std::optional<std::int32_t> gpu_id =
        bb::hle::video::resolve_gpu_id(device_selector, d->window->GetWindowInfo().type, why);
    if (!gpu_id) {
        bb::common::log(LogLevel::error, channel, "--device: {}", why);
        end_run(2);
    }
    bb::shader::set_gpu_id(*gpu_id);
    d->presenter = std::make_unique<bb::hle::video::VideoCorePresenter>(*d->window);
    display = d.release();
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

#ifdef BB_HAVE_MIRROR
// The readable guest dwords from `address` on, to the end of its mapping (at most 1 MiB).
std::span<const std::uint32_t> read_guest_code(std::uint64_t address) {
    constexpr std::uint64_t page = 16384, limit = std::uint64_t{1} << 20;
    std::uint64_t bytes = 0;
    while (bytes < limit) {
        const std::uint64_t next = std::min(limit, ((address + bytes) / page + 1) * page - address);
        if (!bb::hle::kernel::vmem::readable(address, address + next)) {
            break;
        }
        bytes = next;
    }
    return {reinterpret_cast<const std::uint32_t*>(address), bytes / 4};
}

// The guest bytes [address, address + bytes) when they are all readable.
std::span<const std::byte> read_guest_bytes(std::uint64_t address, std::uint64_t bytes) {
    if (address + bytes < address || !bb::hle::kernel::vmem::readable(address, address + bytes)) {
        return {};
    }
    return {reinterpret_cast<const std::byte*>(address), bytes};
}

// Makes the GPU memory mirror over the first `gib` GiB of the guest window and hooks it to direct
// memory, on `device` or, without one, on a device of its own. Logs why and goes on without a
// mirror when it can't.
void open_mirror(bb::vk::Device* device, const std::string& device_selector, unsigned gib) {
    auto m = std::make_unique<Mirror>();
    std::string why;
    try {
        if (device == nullptr) {
            m->instance = bb::vk::Instance::create(why);
            if (m->instance) {
                m->device = bb::vk::Device::create(*m->instance, {.device = device_selector}, why);
            }
            device = m->device.get();
        }
        const int memfd = bb::hle::kernel::dmem::fd();
        if (device != nullptr && memfd < 0) {
            why = "no direct memory";
            device = nullptr;
        }
        if (device != nullptr) {
            m->mirror = bb::vk::MemoryMirror::create(
                *device,
                {.memfd = memfd,
                 .window_start = bb::hle::kernel::vmem::window_start,
                 .size = std::uint64_t{gib} << 30},
                why);
        }
    } catch (const std::runtime_error& e) {
        why = e.what();
    }
    if (!m->mirror) {
        bb::common::log(LogLevel::warn, channel, "no GPU memory mirror: {}", why);
        return;
    }
    m->observer = std::make_unique<bb::tools::MirrorObserver>(*m->mirror);
    bb::hle::kernel::vmem::add_mapping_observer(m->observer.get());
    bb::common::log(LogLevel::info, channel, "GPU memory mirror of {} GiB, blocks of {:#x}", gib,
                    m->mirror->block());
    // Dispatches run through the mirror on the GPU thread.
    try {
        m->scheduler = std::make_unique<bb::vk::Scheduler>(*device);
        m->pipelines = std::make_unique<bb::vk::ComputePipelineCache>(*device, read_guest_code);
        m->images = std::make_unique<bb::vk::ImageCache>(*device);
        m->targets = std::make_unique<bb::vk::TargetRenderer>(*device, *m->images);
        m->draw_pipelines = std::make_unique<bb::vk::PipelineCache>(*device, read_guest_code);
        m->textures = std::make_unique<bb::vk::TextureCache>(*device, *m->scheduler);
        m->binder = std::make_unique<bb::vk::TextureBinder>(*device, *m->scheduler, *m->textures,
                                                            read_guest_bytes);
        m->dispatcher = std::make_unique<bb::vk::Dispatcher>(
            *device, *m->scheduler, *m->pipelines, *m->mirror, m->binder.get());
        auto& gpu = bb::hle::video::gpu_thread();
        m->recorder = std::make_unique<bb::vk::DrawRecorder>(
            *device, *m->scheduler, *m->dispatcher, *m->draw_pipelines, *m->targets, *m->mirror,
            gpu.extent_hints(), m->binder.get());
        Mirror* const raw = m.get();
        gpu.set_backend(
            [raw](const bb::pm4::RegisterMirror& regs, const bb::pm4::DispatchCall& call) {
                raw->dispatcher->dispatch(regs, call);
            },
            [raw](bb::pm4::SyncPoint point) { raw->dispatcher->sync(point); },
            [raw](const bb::pm4::RegisterMirror& regs, const bb::pm4::DrawCall& call) {
                raw->recorder->draw(regs, call);
            });
    } catch (const std::runtime_error& e) {
        bb::common::log(LogLevel::warn, channel, "no GPU dispatches or draws: {}", e.what());
    }
    mirror = m.release();
}
#endif

// Everything up to the start of the game's main thread. Returns the exit status of a run that
// ends before it; throws for errors that end it with status 1.
int start(const bb::tools::RunOptions& options) {
    const auto start_time = std::chrono::steady_clock::now();
    if (options.log_spec) {
        bb::common::set_log_filter(bb::common::LogFilter::parse(*options.log_spec));
    } else {
        bb::common::set_log_filter_from_environment();  // before link: the call trace reads it
    }
    std::optional<fs::path> game_dir = options.game_dir;
    if (!game_dir) {
        game_dir = bb::common::game_dir("local.env");
    }
    if (!game_dir) {
        std::cerr << "bb-run: set BB_GAME_DIR (environment or ./local.env) or pass --game-dir\n";
        return 2;
    }
    game_dir = fs::absolute(*game_dir);
    const bb::common::RuntimeDirs dirs =
        bb::common::RuntimeDirs::from_environment(options.profile, {*game_dir});
    if (options.log_file) {
        const fs::path log = bb::common::open_log_file(dirs);
        bb::common::log(LogLevel::info, channel, "log file {}", log.string());
    }
    bb::common::log(LogLevel::info, channel, "game {}, profile {}", game_dir->string(),
                    options.profile);
    if (options.gpu_trace) {
        const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        const fs::path trace = dirs.ensure(dirs.logs()) /
                               std::format("{:%Y%m%dT%H%M%S}Z-{}-gpu-trace.txt", now, ::getpid());
        if (!bb::hle::video::enable_gpu_trace(trace)) {
            std::cerr << "bb-run: cannot create the GPU trace " << trace.string() << '\n';
            return 1;
        }
        bb::common::log(LogLevel::info, channel, "GPU trace {}", trace.string());
    }

#ifdef BB_HAVE_VIDEO_CORE
    // video_core's guest memory follows the kernel's mappings from the first one on. Its write
    // tracking needs the fault handler (docs/design/renderer.md).
    guest_memory = Core::Memory::Instance();
    bb::shadps4::install_fault_handler();
#endif
    // Never destroyed: the game runs in it until the process ends.
    auto* game = new Game{bb::loader::map_load_set(*game_dir)};
    game->eboot = &bb::hle::prepare(game->set, [](bb::loader::Image& eboot) {
        bb::hook::apply(eboot, bb::hook::replacements());
        bb::common::log(LogLevel::info, channel, "{} hooks applied",
                        bb::hook::replacements().size());
    });
    bb::game::set_image(game->eboot);
    bb::hle::kernel::check_eboot_tls();

    bb::hle::kernel::path_map().mount_game(*game_dir);
    bb::hle::system::set_save_data_root(bb::hle::system::save_data_root(dirs));
    bb::hle::system::set_user_name(options.profile);
    main_loop = new MainLoop{
        bb::tools::MainLoopProbe(
            game->eboot->runtime(bb::tools::frame_pacer),
            game->eboot->runtime(bb::tools::frame_pacer + bb::tools::frame_pacer_size)),
        options.stop_at_main_loop, start_time};
    bb::hle::kernel::set_gettimeofday_observer(&observe_gettimeofday);
#ifdef BB_HAVE_WINDOW
    if (!options.headless) {
        open_display(options.device);
    }
    bb::hle::video::start_video_out(display != nullptr ? display->presenter.get() : nullptr);
#else
    bb::hle::video::start_video_out(nullptr);
#endif
#ifdef BB_HAVE_MIRROR
    open_mirror(nullptr, options.device, options.mirror_gib);
#endif
    run_game(*game, options.timeout_seconds);
}

}  // namespace

int main(int argc, char** argv) {
    bb::tools::RunOptions options;
    try {
        const std::vector<std::string_view> args(argv + 1, argv + argc);
        options = bb::tools::parse_run_options(args);
    } catch (const std::invalid_argument& e) {
        std::cerr << "bb-run: " << e.what() << '\n' << bb::tools::run_usage;
        return 2;
    }
    if (options.help) {
        std::cout << bb::tools::run_usage;
        return 0;
    }
    bb::common::set_thread_name("bb-run");
    try {
        return start(options);
    } catch (const std::exception& e) {
        bb::common::log(LogLevel::error, channel, "{}", e.what());
        end_run(1);
    }
}
