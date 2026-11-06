// bb-run: loads the game and runs it from its entry point (docs/design/runtime.md, "Tools";
// docs/design/m3-core-os.md, "Entry").
//
// The initial thread sets up the runtime, maps and links the load set, applies the registered
// hooks and starts the game's main thread. That thread runs the PRX initialisers and then the
// eboot's entry. The initial thread only waits for it, or for --timeout. The main-loop probe logs
// when the game reaches its main loop and, with --stop-at-main-loop, ends the run there, on the
// game's main thread. Every way out after the
// load goes through end_run, never std::exit: game threads may still run, and host atexit
// handlers, static destructors and LeakSanitizer must not run under them.

#include <pthread.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <exception>
#include <filesystem>
#include <format>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "common/env.hpp"
#include "common/log.hpp"
#include "common/runtime_dirs.hpp"
#include "game/image.hpp"
#include "hle/kernel/paths.hpp"
#include "hle/kernel/thread.hpp"
#include "hle/kernel/time.hpp"
#include "hle/kernel/tls.hpp"
#include "hle/link.hpp"
#include "hle/prx_init.hpp"
#include "hle/system/save_data.hpp"
#include "hle/system/system.hpp"
#include "hook/hook.hpp"
#include "loader/load_set.hpp"
#include "loader/relocate.hpp"
#include "tools/main_loop_probe.hpp"
#include "tools/run_options.hpp"

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

// Flushes stdio and the log and ends the process without host atexit handlers or static
// destructors, as the HLE _exit does.
[[noreturn]] void end_run(int status) {
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

// Starts game_main on a thread of its own and waits for it, at most `timeout_seconds`.
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
    if (!timeout_seconds) {
        pthread_join(thread, nullptr);
        end_run(0);  // not reached: game_main ends the process
    }
    timespec deadline{};
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += *timeout_seconds;
    while (true) {
        const int err = pthread_clockjoin_np(thread, nullptr, CLOCK_MONOTONIC, &deadline);
        if (err == ETIMEDOUT) {
            break;
        }
        if (err != EINTR) {
            end_run(0);  // not reached: game_main ends the process
        }
    }
    bb::common::log(LogLevel::error, channel, "timeout: the game still runs after {} s",
                    *timeout_seconds);
    end_run(bb::tools::timeout_status);
}

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

    // Never destroyed: the game runs in it until the process ends.
    auto* game = new Game{bb::loader::map_load_set(*game_dir)};
    bb::hle::link(game->set);
    for (bb::loader::LoadedModule& m : game->set.modules) {
        if (m.id == bb::loader::eboot_module_id) {
            game->eboot = &m.image;
        }
    }
    if (game->eboot == nullptr) {
        throw std::logic_error("the load set has no eboot");
    }
    bb::hook::apply(*game->eboot, bb::hook::replacements());
    bb::common::log(LogLevel::info, channel, "{} hooks applied", bb::hook::replacements().size());
    for (bb::loader::LoadedModule& m : game->set.modules) {
        m.image.finalize_protection();
    }
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
