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
    EXPECT_FALSE(o.gpu_trace);
    EXPECT_EQ(o.mirror_gib, 32U);
    EXPECT_EQ(o.device, "");
    EXPECT_FALSE(o.help);
}

TEST(RunOptions, ParsesEveryOption) {
    const RunOptions o = parse({"--game-dir", "/games/bb", "--profile", "test_1-a", "--timeout",
                                "10", "--stop-at-main-loop", "--log", "hle=trace,warn",
                                "--no-log-file", "--headless", "--device", "RADV", "--gpu-trace",
                                "--mirror-size", "8"});
    EXPECT_EQ(o.game_dir, std::filesystem::path("/games/bb"));
    EXPECT_EQ(o.profile, "test_1-a");
    EXPECT_EQ(o.timeout_seconds, 10U);
    EXPECT_TRUE(o.stop_at_main_loop);
    EXPECT_EQ(o.log_spec, "hle=trace,warn");
    EXPECT_TRUE(o.gpu_trace);
    EXPECT_FALSE(o.log_file);
    EXPECT_TRUE(o.headless);
    EXPECT_EQ(o.device, "RADV");
    EXPECT_EQ(o.mirror_gib, 8U);
    EXPECT_TRUE(parse({"-h"}).help);
    EXPECT_TRUE(parse({"--help"}).help);
    // A later value replaces an earlier one.
    EXPECT_EQ(parse({"--timeout", "1", "--timeout", "86400"}).timeout_seconds, 86400U);
}

TEST(RunOptions, RefusesBadArguments) {
    EXPECT_EQ(parse_error({"--frobnicate"}), "unknown argument \"--frobnicate\"");
    EXPECT_EQ(parse_error({"eboot.bin"}), "unknown argument \"eboot.bin\"");
    EXPECT_EQ(parse_error({"--profile"}), "--profile needs a value");
    EXPECT_EQ(parse_error({"--game-dir"}), "--game-dir needs a value");
    EXPECT_EQ(parse_error({"--log"}), "--log needs a value");
    EXPECT_EQ(parse_error({"--timeout"}), "--timeout needs a value");
    EXPECT_EQ(parse_error({"--device"}), "--device needs a value");
    EXPECT_EQ(parse_error({"--mirror-size"}), "--mirror-size needs a value");
    for (std::string_view g : {"0", "1", "3", "34", "64", "-2", "2G", "", "2.0"}) {
        EXPECT_TRUE(parse_error({"--mirror-size", g}).starts_with("--mirror-size: ")) << g;
    }
    EXPECT_EQ(parse({"--mirror-size", "2"}).mirror_gib, 2U);
    EXPECT_EQ(parse({"--mirror-size", "32"}).mirror_gib, 32U);
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

TEST(MainLoopProbe, CoversTheFramePacer) {
    EXPECT_EQ(bb::tools::frame_pacer + bb::tools::frame_pacer_size, 0x2034fbcU);
    EXPECT_EQ(bb::tools::main_loop_calls, 60U);
}

// A synthetic range [0x5000, 0x5100) and a threshold of 3.
TEST(MainLoopProbe, CountsReturnAddressesInsideTheRange) {
    MainLoopProbe probe(0x5000, 0x5100, 3);
    for (const std::uintptr_t outside : {0x4fffU, 0x5100U, 0U, 0x12345678U}) {
        EXPECT_FALSE(probe.count(outside)) << outside;
    }
    EXPECT_EQ(probe.calls(), 0U);
    EXPECT_FALSE(probe.count(0x5000));  // the first byte counts
    EXPECT_FALSE(probe.count(0x50ff));  // and the last
    EXPECT_EQ(probe.calls(), 2U);
    EXPECT_TRUE(probe.count(0x5080));  // the third reaches the threshold
    EXPECT_EQ(probe.calls(), 3U);
    // Only once: later calls don't report again, and stop counting.
    for (int i = 0; i < 10; ++i) {
        EXPECT_FALSE(probe.count(0x5080));
    }
    EXPECT_EQ(probe.calls(), 3U);
}

TEST(MainLoopProbe, StopsCountingOnceMainHasReturned) {
    MainLoopProbe probe(0x5000, 0x5100, 2);
    EXPECT_FALSE(probe.count(0x5010));
    probe.main_returned();
    EXPECT_FALSE(probe.count(0x5010));
    EXPECT_FALSE(probe.count(0x5010));
    EXPECT_EQ(probe.calls(), 1U);
}

TEST(MainLoopProbe, ReportsExactlyOnceAcrossThreads) {
    MainLoopProbe probe(0x5000, 0x5100);  // the default threshold, 60
    std::atomic<int> reports{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 100; ++i) {
                reports += probe.count(0x5040) ? 1 : 0;
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

// --gpu-trace creates its file next to the log file, and the run says where.
TEST(BbRunGame, CreatesTheGpuTrace) {
    BB_REQUIRE_GAME(game);
    const TempDir dir;
    const Result r = run_bb_run(dir, "--headless --gpu-trace --timeout 10 --profile test "
                                     "--game-dir " +
                                         quote(game.string()));
    ASSERT_NE(r.status, -1) << r.output;
    EXPECT_NE(r.output.find("GPU trace "), std::string::npos) << r.output;
    int traces = 0;
    for (const auto& entry :
         std::filesystem::directory_iterator(dir.path() / "state/bloodborne-linux/logs")) {
        traces += entry.path().string().ends_with("-gpu-trace.txt") ? 1 : 0;
    }
    EXPECT_EQ(traces, 1) << r.output;
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

// The GPU memory mirror follows the game's direct mappings: the run binds them and ends cleanly
// (a machine without a suitable Vulkan device logs why and runs without a mirror).
TEST(BbRunGame, MirrorsDirectMemory) {
    BB_REQUIRE_GAME(game);
    const TempDir dir;
    const Result r = run_bb_run(dir, "--headless --mirror-size 4 --timeout 10 --profile test "
                                     "--game-dir " +
                                         quote(game.string()));
    ASSERT_NE(r.status, -1) << r.output;
    if (r.output.find("no GPU memory mirror: ") != std::string::npos) {
        GTEST_SKIP() << "no mirror on this machine";
    }
    EXPECT_NE(r.output.find("GPU memory mirror of 4 GiB"), std::string::npos) << r.output;
    EXPECT_NE(r.output.find(" binds, "), std::string::npos) << r.output;
    EXPECT_EQ(r.output.find("run: mirror: 0 binds"), std::string::npos) << r.output;
    // No error line of the mirror. Lines start with a clock stamp: "23:57:19.639 error [game main]
    // mirror: ...".
    std::istringstream lines(r.output);
    for (std::string line; std::getline(lines, line);) {
        EXPECT_FALSE(line.find(" error [") != std::string::npos && line.find("] mirror: ") != std::string::npos)
            << line;
    }
}

}  // namespace
