// Runs bb-launcher on SDL's offscreen video driver with a script of steps (see
// src/launcher/logic.hpp) and looks at what it writes.
#ifdef BB_LAUNCHER_PATH

#include <stdlib.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>

#include <gtest/gtest.h>

#include "support/game.hpp"
#include "support/process.hpp"

namespace {

namespace fs = std::filesystem;
using bb::test::quote;

class Dir {
public:
    Dir() {
        std::string pattern = (fs::temp_directory_path() / "bb-launcher-test-XXXXXX").string();
        if (mkdtemp(pattern.data()) == nullptr) {
            throw std::runtime_error("mkdtemp failed");
        }
        path_ = pattern;
    }
    ~Dir() { fs::remove_all(path_); }
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

bb::test::Result run_script(const Dir& dir, const std::string& script) {
    std::ofstream(dir.path() / "script.txt") << script;
    const std::string d = dir.path().string();
    return bb::test::run(std::format(
        "cd {} && env -u BB_GAME_DIR -u WAYLAND_DISPLAY -u DISPLAY LC_ALL=C ASAN_OPTIONS=detect_leaks=0 "
        "SDL_VIDEO_DRIVER=offscreen SDL_RENDER_DRIVER=software BB_LAUNCHER_SCRIPT={} "
        "XDG_CONFIG_HOME={} XDG_DATA_HOME={} XDG_CACHE_HOME={} timeout 60 {} 2>&1",
        quote(d), quote(d + "/script.txt"), quote(d + "/config"), quote(d + "/data"),
        quote(d + "/cache"), quote(BB_LAUNCHER_PATH)));
}

bool is_png(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    char magic[4] = {};
    in.read(magic, 4);
    return in && magic[1] == 'P' && magic[2] == 'N' && magic[3] == 'G';
}

// A folder that fails its check is shown in the status line and nothing is written.
TEST(LauncherRun, ARefusedFolderWritesNoConfig) {
    const Dir dir;
    fs::create_directories(dir.path() / "empty");
    const auto r = run_script(dir, std::format("wait 3\ndrop {}\nshot {}\nquit\n",
                                               (dir.path() / "empty").string(),
                                               (dir.path() / "shot.png").string()));
    EXPECT_EQ(r.status, 0) << r.output;
    EXPECT_TRUE(is_png(dir.path() / "shot.png"));
    EXPECT_FALSE(fs::exists(dir.path() / "config"));
}

std::string slurp(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Runs a copy of bb-launcher next to a fake bb-run that records its arguments and directory and
// exits with `exit_code`. The script drops the dump, presses Play and takes a shot afterwards.
std::string play_with_fake_bb_run(const Dir& dir, const fs::path& game, int exit_code,
                                  bb::test::Result& result) {
    const fs::path bin = dir.path() / "bin";
    fs::create_directories(bin);
    fs::copy_file(BB_LAUNCHER_PATH, bin / "bb-launcher");
    std::ofstream(bin / "bb-run") << std::format(
        "#!/bin/sh\necho \"cwd=$(pwd) args=$*\" > {}\nexit {}\n",
        quote((dir.path() / "record.txt").string()), exit_code);
    fs::permissions(bin / "bb-run", fs::perms::owner_all);
    std::ofstream(dir.path() / "script.txt") << std::format(
        "wait 3\ndrop {}\nwait 3\npress accept\nwaitgame 20\nwait 3\nshot {}\nquit\n",
        game.string(), (dir.path() / "shot.png").string());
    const std::string d = dir.path().string();
    result = bb::test::run(std::format(
        "cd {} && env -u BB_GAME_DIR -u WAYLAND_DISPLAY -u DISPLAY LC_ALL=C ASAN_OPTIONS=detect_leaks=0 "
        "SDL_VIDEO_DRIVER=offscreen SDL_RENDER_DRIVER=software BB_LAUNCHER_SCRIPT={} "
        "XDG_CONFIG_HOME={} XDG_DATA_HOME={} XDG_CACHE_HOME={} timeout 60 {} 2>&1",
        quote(d), quote(d + "/script.txt"), quote(d + "/config"), quote(d + "/data"),
        quote(d + "/cache"), quote((bin / "bb-launcher").string())));
    return slurp(dir.path() / "record.txt");
}

// Play starts bb-run from the launcher's directory with only --config, and the exit status
// decides the screen that follows: a failed run looks different from a good one.
TEST(LauncherRunGame, PlayStartsBbRunWithOnlyTheConfig) {
    BB_REQUIRE_GAME(game);
    const Dir good;
    const Dir bad;
    bb::test::Result rg, rb;
    const std::string rec = play_with_fake_bb_run(good, game, 0, rg);
    const std::string rec_bad = play_with_fake_bb_run(bad, game, 3, rb);
    EXPECT_EQ(rg.status, 0) << rg.output;
    EXPECT_EQ(rb.status, 0) << rb.output;
    const std::string config = (good.path() / "config/bloodborne-linux/config.toml").string();
    EXPECT_EQ(rec, std::format("cwd={} args=--config {}\n", (good.path() / "bin").string(), config));
    EXPECT_NE(rec_bad.find("args=--config "), std::string::npos);
    EXPECT_NE(slurp(good.path() / "shot.png"), slurp(bad.path() / "shot.png"));
}

// The dump: the folder goes into the config file, only in the XDG config directory.
TEST(LauncherRunGame, TheDumpFolderIsWrittenToTheConfig) {
    BB_REQUIRE_GAME(game);
    const Dir dir;
    const auto r = run_script(dir, std::format("wait 3\ndrop {}\nwait 3\nshot {}\nquit\n",
                                               game.string(), (dir.path() / "shot.png").string()));
    EXPECT_EQ(r.status, 0) << r.output;
    EXPECT_TRUE(is_png(dir.path() / "shot.png"));
    const fs::path config = dir.path() / "config/bloodborne-linux/config.toml";
    ASSERT_TRUE(fs::exists(config));
    EXPECT_NE(slurp(config).find(fs::canonical(game).string()), std::string::npos);
    EXPECT_FALSE(fs::exists(dir.path() / "data"));
}

TEST(LauncherRun, ABadScriptEndsWithAMessage) {
    const Dir dir;
    const auto r = run_script(dir, "frobnicate\n");
    EXPECT_EQ(r.status, 1);
    EXPECT_NE(r.output.find("line 1"), std::string::npos) << r.output;
}

}  // namespace

#endif
