// Runs bb-launcher on SDL's offscreen video driver with a script of steps (see
// src/launcher/logic.hpp) and looks at what it writes.
#ifdef BB_LAUNCHER_PATH

#include <stdlib.h>

#include <filesystem>
#include <format>
#include <fstream>
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

// The dump: the folder goes into the config file, only in the XDG config directory.
TEST(LauncherRunGame, TheDumpFolderIsWrittenToTheConfig) {
    BB_REQUIRE_GAME(game);
    const Dir dir;
    const auto r = run_script(dir, std::format("wait 3\ndrop {}\nwait 3\nshot {}\nquit\n",
                                               game.string(), (dir.path() / "shot.png").string()));
    EXPECT_EQ(r.status, 0) << r.output;
    EXPECT_TRUE(is_png(dir.path() / "shot.png"));
    EXPECT_TRUE(fs::exists(dir.path() / "config/bloodborne-linux/config.toml"));
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
