// bb-run's command line and entry arguments (src/tools/run_options.hpp), its main-loop probe
// (src/tools/main_loop_probe.hpp), and the bb-run binary itself, without the game and on it.

#include <stdlib.h>
#include <sys/wait.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "support/game.hpp"
#include "support/process.hpp"
#include "tools/main_loop_probe.hpp"
#include "tools/run_options.hpp"

namespace {

using bb::tools::MainLoopProbe;
using bb::tools::parse_run_options;
using bb::tools::PresentMode;
using bb::tools::RunOptions;

RunOptions parse(std::vector<std::string_view> args) {
    return parse_run_options(args);
}

// The message of the std::invalid_argument that parsing `args` throws.
std::string parse_error(std::vector<std::string_view> args) {
    try {
        parse(args);
    } catch (const std::invalid_argument& e) {
        return e.what();
    }
    return "no error";
}

TEST(RunOptions, Defaults) {
    const RunOptions o = parse({});
    EXPECT_FALSE(o.game_dir);
    EXPECT_EQ(o.profile, "default");
    EXPECT_FALSE(o.timeout_seconds);
    EXPECT_FALSE(o.stop_at_main_loop);
    EXPECT_FALSE(o.log_spec);
    EXPECT_TRUE(o.log_file);
    EXPECT_FALSE(o.headless);
    EXPECT_EQ(o.device, "");
    EXPECT_FALSE(o.validation);
    EXPECT_FALSE(o.validation_sync);
    EXPECT_FALSE(o.validation_gpu);
    EXPECT_EQ(o.present_mode, PresentMode::mailbox);
    EXPECT_TRUE(o.pipeline_cache);
    EXPECT_TRUE(o.async_pipelines);
    EXPECT_FALSE(o.help);
}

TEST(RunOptions, NoPipelineCacheTurnsTheDiskCacheOff) {
    EXPECT_FALSE(parse({"--no-pipeline-cache"}).pipeline_cache);
}

TEST(RunOptions, SyncPipelinesBuildsOnTheGpuThread) {
    const RunOptions o = parse({"--sync-pipelines"});
    EXPECT_FALSE(o.async_pipelines);
    EXPECT_TRUE(o.pipeline_cache);
}

TEST(RunOptions, ParsesEveryOption) {
    const RunOptions o = parse({"--game-dir", "/games/bb", "--profile", "test_1-a", "--timeout",
                                "10", "--stop-at-main-loop", "--log", "hle=trace,warn",
                                "--no-log-file", "--headless", "--device", "RADV"});
    EXPECT_EQ(o.game_dir, std::filesystem::path("/games/bb"));
    EXPECT_EQ(o.profile, "test_1-a");
    EXPECT_EQ(o.timeout_seconds, 10U);
    EXPECT_TRUE(o.stop_at_main_loop);
    EXPECT_EQ(o.log_spec, "hle=trace,warn");
    EXPECT_FALSE(o.log_file);
    EXPECT_TRUE(o.headless);
    EXPECT_EQ(o.device, "RADV");
    EXPECT_TRUE(parse({"-h"}).help);
    EXPECT_TRUE(parse({"--help"}).help);
    // A later value replaces an earlier one.
    EXPECT_EQ(parse({"--timeout", "1", "--timeout", "86400"}).timeout_seconds, 86400U);
}

TEST(RunOptions, ParsesTheRendererOptions) {
    const RunOptions o = parse({"--validation", "--validation-sync", "--validation-gpu",
                                "--present-mode", "fifo"});
    EXPECT_TRUE(o.validation);
    EXPECT_TRUE(o.validation_sync);
    EXPECT_TRUE(o.validation_gpu);
    EXPECT_EQ(o.present_mode, PresentMode::fifo);
    EXPECT_EQ(parse({"--present-mode", "immediate"}).present_mode, PresentMode::immediate);
    EXPECT_EQ(parse({"--present-mode", "mailbox"}).present_mode, PresentMode::mailbox);
    EXPECT_EQ(parse_error({"--present-mode", "vsync"}),
              "--present-mode: expected fifo, mailbox or immediate");
    EXPECT_EQ(parse_error({"--present-mode"}), "--present-mode needs a value");
    EXPECT_EQ(parse_error({"--validation-sync"}),
              "--validation-sync and --validation-gpu need --validation");
    EXPECT_EQ(parse_error({"--validation-gpu"}),
              "--validation-sync and --validation-gpu need --validation");
}

TEST(RunOptions, ParsesThePadInput) {
    EXPECT_FALSE(parse({}).pad_input);
    EXPECT_EQ(parse({"--pad-input", "/run/pad"}).pad_input, std::filesystem::path("/run/pad"));
    EXPECT_EQ(parse_error({"--pad-input"}), "--pad-input needs a value");
}

TEST(RunOptions, ParsesTheFrameRate) {
    EXPECT_EQ(parse({}).frame_rate, 30U);
    EXPECT_EQ(parse({"--frame-rate", "60"}).frame_rate, 60U);
    EXPECT_EQ(parse({"--frame-rate", "60", "--frame-rate", "30"}).frame_rate, 30U);
    EXPECT_EQ(parse_error({"--frame-rate"}), "--frame-rate needs a value");
    for (std::string_view fps : {"0", "45", "120", "60.0", " 60", "", "sixty"}) {
        EXPECT_EQ(parse_error({"--frame-rate", fps}),
                  std::format("--frame-rate: \"{}\" is not 30 or 60", fps));
    }
}

TEST(RunOptions, ParsesTheWindowOptions) {
    const RunOptions d = parse({});
    EXPECT_EQ(d.window_width, 1280U);
    EXPECT_EQ(d.window_height, 720U);
    EXPECT_FALSE(d.fullscreen);
    const RunOptions o = parse({"--window-size", "2560x1440", "--fullscreen"});
    EXPECT_EQ(o.window_width, 2560U);
    EXPECT_EQ(o.window_height, 1440U);
    EXPECT_TRUE(o.fullscreen);
    const RunOptions edge = parse({"--window-size", "640x360", "--window-size", "16384x16384"});
    EXPECT_EQ(edge.window_width, 16384U);
    EXPECT_EQ(edge.window_height, 16384U);
    EXPECT_EQ(parse({"--window-size", "640x360"}).window_height, 360U);
    EXPECT_EQ(parse_error({"--window-size"}), "--window-size needs a value");
    for (std::string_view size : {"639x360", "640x359", "16385x720", "1280x16385", "1280", "x720",
                                  "1280x", "1280X720", "1280x720x1", "+1280x720", " 1280x720",
                                  "1280.0x720", "-1x720", ""}) {
        EXPECT_EQ(parse_error({"--window-size", size}),
                  std::format("--window-size: \"{}\" is not WIDTHxHEIGHT from 640x360 to "
                              "16384x16384",
                              size));
    }
}

TEST(RunOptions, ParsesFsrAndRcas) {
    const RunOptions d = parse({});
    EXPECT_FALSE(d.fsr);
    EXPECT_TRUE(d.rcas);
    EXPECT_FLOAT_EQ(d.rcas_attenuation, 0.25f);
    const RunOptions fsr = parse({"--fsr"});
    EXPECT_TRUE(fsr.fsr);
    EXPECT_TRUE(fsr.rcas);
    EXPECT_FLOAT_EQ(fsr.rcas_attenuation, 0.25f);
    EXPECT_FALSE(parse({"--fsr", "--no-rcas"}).rcas);
    EXPECT_FLOAT_EQ(parse({"--rcas-attenuation", "1.5", "--fsr"}).rcas_attenuation, 1.5f);
    EXPECT_FLOAT_EQ(parse({"--fsr", "--rcas-attenuation", "0"}).rcas_attenuation, 0.0f);
    EXPECT_FLOAT_EQ(parse({"--fsr", "--rcas-attenuation", "2"}).rcas_attenuation, 2.0f);
    EXPECT_EQ(parse_error({"--no-rcas"}), "--no-rcas and --rcas-attenuation need --fsr");
    EXPECT_EQ(parse_error({"--rcas-attenuation", "1"}),
              "--no-rcas and --rcas-attenuation need --fsr");
    EXPECT_EQ(parse_error({"--fsr", "--rcas-attenuation"}), "--rcas-attenuation needs a value");
    for (std::string_view stops : {"-0.1", "2.01", "nan", "inf", "1,5", "0.5s", "", " 1"}) {
        EXPECT_EQ(parse_error({"--fsr", "--rcas-attenuation", stops}),
                  std::format("--rcas-attenuation: \"{}\" is not a number from 0 to 2", stops));
    }
}

TEST(RunOptions, RefusesBadArguments) {
    EXPECT_EQ(parse_error({"--frobnicate"}), "unknown argument \"--frobnicate\"");
    EXPECT_EQ(parse_error({"eboot.bin"}), "unknown argument \"eboot.bin\"");
    EXPECT_EQ(parse_error({"--profile"}), "--profile needs a value");
    EXPECT_EQ(parse_error({"--game-dir"}), "--game-dir needs a value");
    EXPECT_EQ(parse_error({"--log"}), "--log needs a value");
    EXPECT_EQ(parse_error({"--timeout"}), "--timeout needs a value");
    EXPECT_EQ(parse_error({"--device"}), "--device needs a value");
    // The options of the removed GPU path are unknown now.
    EXPECT_EQ(parse_error({"--mirror-size", "8"}), "unknown argument \"--mirror-size\"");
    EXPECT_EQ(parse_error({"--gpu-trace"}), "unknown argument \"--gpu-trace\"");
    for (std::string_view name :
         {"", "a/b", "../x", "with space", "abcdefghijklmnopqrstuvwxyz0123456"}) {
        EXPECT_TRUE(parse_error({"--profile", name}).starts_with("--profile: ")) << name;
    }
    for (std::string_view t : {"0", "86401", "-1", "10s", "", "1.5", " 1", "99999999999"}) {
        EXPECT_TRUE(parse_error({"--timeout", t}).starts_with("--timeout: ")) << t;
    }
    EXPECT_TRUE(parse_error({"--log", "hle=loud"}).starts_with("--log: "))
        << parse_error({"--log", "hle=loud"});
}

TEST(EntryArgs, MatchesTheEntrysReads) {
    const bb::tools::EntryArgs args = bb::tools::entry_args();
    // The entry reads a 4-byte argc at +0 and takes argv as the address +8.
    const auto* bytes = reinterpret_cast<const unsigned char*>(&args);
    std::int32_t argc = 0;
    std::memcpy(&argc, bytes, sizeof argc);
    EXPECT_EQ(argc, 1);
    const char* const* argv = reinterpret_cast<const char* const*>(bytes + 8);
    EXPECT_STREQ(argv[0], "/app0/eboot.bin");
    EXPECT_EQ(argv[1], nullptr);
    EXPECT_EQ(sizeof args, 24U);
}

// ---- the main-loop probe ------------------------------------------------------------------------

TEST(MainLoopProbe, ReportsTheThresholdthCall) {
    EXPECT_EQ(bb::tools::main_loop_calls, 60U);
    MainLoopProbe probe(3);
    EXPECT_FALSE(probe.count());
    EXPECT_FALSE(probe.count());
    EXPECT_EQ(probe.calls(), 2U);
    EXPECT_TRUE(probe.count());  // the third reaches the threshold
    EXPECT_EQ(probe.calls(), 3U);
    // Only once: later calls don't report again, and stop counting.
    for (int i = 0; i < 10; ++i) {
        EXPECT_FALSE(probe.count());
    }
    EXPECT_EQ(probe.calls(), 3U);
}

TEST(MainLoopProbe, StopsCountingOnceMainHasReturned) {
    MainLoopProbe probe(2);
    EXPECT_FALSE(probe.count());
    probe.main_returned();
    EXPECT_FALSE(probe.count());
    EXPECT_FALSE(probe.count());
    EXPECT_EQ(probe.calls(), 1U);
}

TEST(MainLoopProbe, ReportsExactlyOnceAcrossThreads) {
    MainLoopProbe probe;  // the default threshold, 60
    std::atomic<int> reports{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 100; ++i) {
                reports += probe.count() ? 1 : 0;
            }
        });
    }
    for (std::thread& t : threads) {
        t.join();
    }
    EXPECT_EQ(reports, 1);
    EXPECT_GE(probe.calls(), bb::tools::main_loop_calls);
}

// ---- the binary ---------------------------------------------------------------------------------

using bb::test::quote;
using bb::test::Result;

// A fresh directory outside the checkout (RuntimeDirs refuses directories inside it), removed at
// the end of the test.
class TempDir {
public:
    TempDir() {
        std::string pattern =
            (std::filesystem::temp_directory_path() / "bb-run-test-XXXXXX").string();
        if (mkdtemp(pattern.data()) == nullptr) {
            throw std::runtime_error("mkdtemp failed");
        }
        path_ = pattern;
    }
    ~TempDir() { std::filesystem::remove_all(path_); }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

// Runs bb-run with `args` in `dir`, with the XDG base directories below it and without BB_LOG
// and BB_GAME_DIR. Captures its standard output and standard error.
// `env_args` go to env(1) before the XDG variables, e.g. "-u DISPLAY".
Result run_bb_run(const TempDir& dir, const std::string& args, const std::string& env_args = "") {
    const std::string d = dir.path().string();
    return bb::test::run(std::format("cd {} && env -u BB_LOG -u BB_GAME_DIR {} XDG_DATA_HOME={} "
                                     "XDG_CACHE_HOME={} XDG_CONFIG_HOME={} XDG_STATE_HOME={} {} {} "
                                     "2>&1",
                                     quote(d), env_args, quote(d + "/data"), quote(d + "/cache"),
                                     quote(d + "/config"), quote(d + "/state"),
                                     quote(BB_RUN_PATH), args));
}

TEST(BbRun, PrintsTheUsage) {
    const TempDir dir;
    const Result help = run_bb_run(dir, "--help");
    EXPECT_EQ(help.status, 0);
    EXPECT_EQ(help.output, bb::tools::run_usage);
    const Result bad = run_bb_run(dir, "--frobnicate");
    EXPECT_EQ(bad.status, 2);
    EXPECT_TRUE(bad.output.starts_with("bb-run: unknown argument \"--frobnicate\"\nusage: "))
        << bad.output;
}

TEST(BbRun, NeedsAGameDirectory) {
    const TempDir dir;  // no local.env in it, no BB_GAME_DIR
    const Result r = run_bb_run(dir, "");
    EXPECT_EQ(r.status, 2);
    EXPECT_NE(r.output.find("set BB_GAME_DIR"), std::string::npos) << r.output;
    EXPECT_FALSE(std::filesystem::exists(dir.path() / "state")) << "no log file before the game";
}

// --stop-at-main-loop is accepted, and still needs the game.
TEST(BbRun, AcceptsStopAtMainLoop) {
    const TempDir dir;
    const Result r = run_bb_run(dir, "--stop-at-main-loop");
    EXPECT_EQ(r.status, 2);
    EXPECT_NE(r.output.find("set BB_GAME_DIR"), std::string::npos) << r.output;
}

// The whole log file in the logs directory below `dir`, which must hold exactly one.
std::string read_log_file(const TempDir& dir) {
    const std::filesystem::path logs = dir.path() / "state/bloodborne-linux/logs";
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(logs)) {
        files.push_back(entry.path());
    }
    if (files.size() != 1) {
        return std::format("{} log files", files.size());
    }
    std::ifstream in(files[0]);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

// Starts the game: the PRX init and the entry run, and the eboot's first imports log their calls
// (libSceSysmodule's load of libSceAppContent, the first HLE call of the eboot's init, at its
// 0x2026149). The run ends with the game's status or with the timeout's.
TEST(BbRunGame, RunsTheEntry) {
    BB_REQUIRE_GAME(game);
    const TempDir dir;
    const Result r = run_bb_run(dir, "--headless --timeout 10 --profile test --game-dir " +
                                         quote(game.string()));
    ASSERT_NE(r.status, -1) << r.output;
    EXPECT_NE(r.status, 1) << r.output;
    for (const std::string_view line :
         {"libc.prx: initialised", "run: calling the entry", "profile test",
          "hle.libSceSysmodule: load module 0xb4 (libSceAppContent) (caller 0x12026149)"}) {
        EXPECT_NE(r.output.find(line), std::string::npos) << line << "\n" << r.output;
    }
    if (r.status == bb::tools::timeout_status) {
        EXPECT_NE(r.output.find("timeout: the game still runs after 10 s"), std::string::npos)
            << r.output;
    } else {
        EXPECT_NE(r.output.find(std::format("_exit({})", r.status)), std::string::npos)
            << r.output;
    }
    // The log file has the lines too, and the save data root of the profile exists.
    const std::string log = read_log_file(dir);
    EXPECT_NE(log.find("run: calling the entry"), std::string::npos) << log;
    EXPECT_TRUE(std::filesystem::is_directory(dir.path() /
                                              "data/bloodborne-linux/profiles/test/savedata"));
}

// The goal of M3 (docs/design/m3-core-os.md, "Goal"): the game reaches its main loop, and bb-run
// stops there with status 0.
TEST(BbRunGame, ReachesTheMainLoop) {
    BB_REQUIRE_GAME(game);
    const TempDir dir;
    const Result r = run_bb_run(dir, "--headless --stop-at-main-loop --timeout 120 --profile test "
                                     "--game-dir " +
                                         quote(game.string()));
    EXPECT_EQ(r.status, 0) << r.output;
    EXPECT_NE(r.output.find("run: main loop reached after "), std::string::npos) << r.output;
    EXPECT_NE(read_log_file(dir).find("run: main loop reached after "), std::string::npos);
}

// Without a display bb-run runs headless instead of failing, and --device then chooses nothing.
TEST(BbRunGame, RunsHeadlessWithoutADisplay) {
    BB_REQUIRE_GAME(game);
    const TempDir dir;
    const Result r = run_bb_run(dir,
                                "--device 'no such device 0x5ca1ab1e' --timeout 10 --profile test "
                                "--game-dir " +
                                    quote(game.string()),
                                "-u WAYLAND_DISPLAY -u DISPLAY");
    ASSERT_NE(r.status, -1) << r.output;
    EXPECT_NE(r.status, 1) << r.output;
    EXPECT_NE(r.status, 2) << r.output;
    EXPECT_NE(r.output.find(
                  "no window, running headless: neither WAYLAND_DISPLAY nor DISPLAY is set"),
              std::string::npos)
        << r.output;
    EXPECT_NE(r.output.find("run: calling the entry"), std::string::npos) << r.output;
}

// With a display, a --device that names no device ends the run with status 2 before the game
// starts, and says which devices there are.
TEST(BbRunGame, RejectsADeviceThatNamesNoDevice) {
    BB_REQUIRE_GAME(game);
    if (std::getenv("WAYLAND_DISPLAY") == nullptr && std::getenv("DISPLAY") == nullptr) {
        GTEST_SKIP() << "no display";
    }
    const TempDir dir;
    const Result r = run_bb_run(dir,
                                "--device 'no such device 0x5ca1ab1e' --timeout 30 --profile test "
                                "--game-dir " +
                                    quote(game.string()));
    EXPECT_EQ(r.status, 2) << r.output;
    EXPECT_NE(r.output.find("run: --device: no Vulkan device name contains \"no such device "
                            "0x5ca1ab1e\" (devices: "),
              std::string::npos)
        << r.output;
    EXPECT_EQ(r.output.find("run: calling the entry"), std::string::npos) << r.output;
}

}  // namespace
