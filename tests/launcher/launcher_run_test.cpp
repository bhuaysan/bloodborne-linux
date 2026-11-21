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

#include "common/config.hpp"
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
        "cd {} && env -u BB_GAME_DIR -u BB_CACHE_DIR -u WAYLAND_DISPLAY -u DISPLAY LC_ALL=C ASAN_OPTIONS=detect_leaks=0 "
        "SDL_VIDEO_DRIVER=offscreen SDL_RENDER_DRIVER=software BB_LAUNCHER_SCRIPT={} "
        "HOME={} BB_USER_DIR={} timeout 60 {} 2>&1",
        quote(d), quote(d + "/script.txt"), quote(d + "/home"),
        quote(d + "/user"), quote(BB_LAUNCHER_PATH)));
}

// From the main screen to Picture and sound: Settings, then its second entry.
constexpr const char* to_picture =
    "wait 3\npress down\nwait 2\npress accept\nwait 3\npress down\nwait 2\npress accept\nwait 3\n";

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

// Runs a copy of bb-launcher next to a fake bb-run that records its arguments and directory,
// prints one line to stdout and one to stderr and exits with `exit_code`. The script drops the dump, presses Play and takes a shot afterwards.
std::string play_with_fake_bb_run(const Dir& dir, const fs::path& game, int exit_code,
                                  bb::test::Result& result, const std::string& before_play = "",
                                  const std::string& play = "press accept\nwaitgame 20\n") {
    const fs::path bin = dir.path() / "bin";
    fs::create_directories(bin);
    fs::copy_file(BB_LAUNCHER_PATH, bin / "bb-launcher");
    std::ofstream(bin / "bb-run") << std::format(
        "#!/bin/sh\necho \"cwd=$(pwd) args=$*\" > {}\necho fake-run-stdout\necho fake-run-stderr >&2\n"
        "exit {}\n",
        quote((dir.path() / "record.txt").string()), exit_code);
    fs::permissions(bin / "bb-run", fs::perms::owner_all);
    std::ofstream(dir.path() / "script.txt") << std::format(
        "wait 3\ndrop {}\nwait 3\n{}{}wait 3\nshot {}\nquit\n", game.string(), before_play, play,
        (dir.path() / "shot.png").string());
    const std::string d = dir.path().string();
    result = bb::test::run(std::format(
        "cd {} && env -u BB_GAME_DIR -u BB_CACHE_DIR -u WAYLAND_DISPLAY -u DISPLAY LC_ALL=C ASAN_OPTIONS=detect_leaks=0 "
        "SDL_VIDEO_DRIVER=offscreen SDL_RENDER_DRIVER=software BB_LAUNCHER_SCRIPT={} "
        "HOME={} BB_USER_DIR={} timeout 60 {} 2>&1",
        quote(d), quote(d + "/script.txt"), quote(d + "/home"), quote(d + "/user"),
        quote((bin / "bb-launcher").string())));
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
    const std::string config = (good.path() / "user/config.toml").string();
    EXPECT_EQ(rec, std::format("cwd={} args=--config {}\n", (good.path() / "bin").string(), config));
    EXPECT_NE(rec_bad.find("args=--config "), std::string::npos);
    EXPECT_NE(slurp(good.path() / "shot.png"), slurp(bad.path() / "shot.png"));
}

// Back from a page puts the focus on the first button again, so A starts the game.
TEST(LauncherRunGame, PlayWorksAfterAPageWasOpenedAndClosed) {
    BB_REQUIRE_GAME(game);
    const Dir dir;
    bb::test::Result r;
    const std::string rec = play_with_fake_bb_run(
        dir, game, 0, r, "press down\nwait 2\npress accept\nwait 3\npress back\nwait 3\n");
    EXPECT_EQ(r.status, 0) << r.output;
    EXPECT_NE(rec.find("args=--config "), std::string::npos) << r.output;
}

// A refused drop while a game is set up comes up as a prompt, and the same drop again shows it
// again: the shots with the prompt match, the one after OK differs.
TEST(LauncherRunGame, ARefusedDropWhileReadyPromptsEveryTime) {
    BB_REQUIRE_GAME(game);
    const Dir dir;
    const fs::path empty = dir.path() / "empty";
    fs::create_directories(empty);
    const fs::path first = dir.path() / "first.png";
    const fs::path closed = dir.path() / "closed.png";
    const fs::path again = dir.path() / "again.png";
    const auto r = run_script(
        dir, std::format("wait 3\ndrop {}\nwait 3\ndrop {}\nwait 4\nshot {}\npress accept\nwait 4\n"
                         "shot {}\ndrop {}\nwait 4\nshot {}\nquit\n",
                         game.string(), empty.string(), first.string(), closed.string(),
                         empty.string(), again.string()));
    EXPECT_EQ(r.status, 0) << r.output;
    EXPECT_EQ(slurp(first), slurp(again));
    EXPECT_NE(slurp(first), slurp(closed));
}

// Play without a controller asks first: Cancel (B) starts nothing, Play anyway (A on the first
// option) starts the game.
TEST(LauncherRunGame, PlayWithoutAControllerAsksFirst) {
    BB_REQUIRE_GAME(game);
    const Dir cancelled;
    const Dir anyway;
    bb::test::Result rc, ra;
    const std::string rec_cancelled = play_with_fake_bb_run(
        cancelled, game, 0, rc, "nopad\n", "press accept\nwait 3\npress back\nwait 3\n");
    const std::string rec_anyway = play_with_fake_bb_run(
        anyway, game, 0, ra, "nopad\n", "press accept\nwait 3\npress accept\nwaitgame 20\n");
    EXPECT_EQ(rc.status, 0) << rc.output;
    EXPECT_EQ(ra.status, 0) << ra.output;
    EXPECT_EQ(rec_cancelled, "");
    EXPECT_NE(rec_anyway.find("args=--config "), std::string::npos) << ra.output;
}

// The output and the end of the run are kept in user/logs.
TEST(LauncherRunGame, TheRunsOutputAndStatusAreCaptured) {
    BB_REQUIRE_GAME(game);
    const Dir dir;
    bb::test::Result r;
    const fs::path bin = dir.path() / "bin";
    play_with_fake_bb_run(dir, game, 3, r);
    EXPECT_EQ(slurp(dir.path() / "user/logs/launcher-last-run.status"),
              "exit status 3\n");
    const std::string log = slurp(dir.path() / "user/logs/launcher-last-run.log");
    EXPECT_NE(log.find("fake-run-stdout"), std::string::npos) << log;
    EXPECT_NE(log.find("fake-run-stderr"), std::string::npos) << log;
}

// The dump: the folder goes into the config file, only in the user folder.
TEST(LauncherRunGame, TheDumpFolderIsWrittenToTheConfig) {
    BB_REQUIRE_GAME(game);
    const Dir dir;
    const auto r = run_script(dir, std::format("wait 3\ndrop {}\nwait 3\nshot {}\nquit\n",
                                               game.string(), (dir.path() / "shot.png").string()));
    EXPECT_EQ(r.status, 0) << r.output;
    EXPECT_TRUE(is_png(dir.path() / "shot.png"));
    const fs::path config = dir.path() / "user/config.toml";
    ASSERT_TRUE(fs::exists(config));
    EXPECT_NE(slurp(config).find(fs::canonical(game).string()), std::string::npos);
    // Nothing outside the user folder: the launcher's own caches went there too.
    EXPECT_TRUE(!fs::exists(dir.path() / "home") || fs::is_empty(dir.path() / "home"));
}

// The first-start search: a folder of another game under $HOME is no hit, the search ends and
// the drop target stays; nothing is written.
TEST(LauncherRun, TheSearchFindsNothingAndLeavesTheDropTarget) {
    const Dir dir;
    fs::create_directories(dir.path() / "home/Games/Other/sce_sys");
    std::ofstream(dir.path() / "home/Games/Other/eboot.bin") << "x";
    std::ofstream(dir.path() / "home/Games/Other/sce_sys/param.sfo") << "x";
    const auto r = run_script(dir, std::format("wait 60\nshot {}\nquit\n",
                                               (dir.path() / "shot.png").string()));
    EXPECT_EQ(r.status, 0) << r.output;
    EXPECT_NE(r.output.find("search took"), std::string::npos) << r.output;
    EXPECT_NE(r.output.find("0 hit(s)"), std::string::npos) << r.output;
    EXPECT_TRUE(is_png(dir.path() / "shot.png"));
    EXPECT_FALSE(fs::exists(dir.path() / "config"));
}

// With the dump under $HOME (its files linked into a temporary home, as the search doesn't follow
// directory links) the launcher takes it on its own, without a drop or a dialog.
TEST(LauncherRunGame, TheSearchTakesTheDumpUnderHome) {
    BB_REQUIRE_GAME(game);
    const Dir dir;
    const fs::path folder = dir.path() / "home/Games/Bloodborne";
    fs::create_directories(folder / "sce_sys");
    fs::create_symlink(fs::canonical(game) / "eboot.bin", folder / "eboot.bin");
    fs::create_symlink(fs::canonical(game) / "sce_sys/param.sfo", folder / "sce_sys/param.sfo");
    const auto r = run_script(dir, std::format("wait 400\nshot {}\nquit\n",
                                               (dir.path() / "shot.png").string()));
    EXPECT_EQ(r.status, 0) << r.output;
    EXPECT_NE(r.output.find("1 hit(s)"), std::string::npos) << r.output;
    const fs::path config = dir.path() / "user/config.toml";
    ASSERT_TRUE(fs::exists(config));
    // Stored relative here (home/ and user/ share the test folder); read back it is the dump.
    EXPECT_EQ(bb::common::read_config(config).config.game_dir, fs::canonical(folder));
}

// The portable folder: a dump in game/ next to the launcher is taken without any setup and stored
// relative to the folder, so the folder still works after it was moved. No BB_USER_DIR: user/ is
// next to the copied launcher, outside the checkout.
TEST(LauncherRunGame, TheDumpInGameIsTakenAndTheFolderCanMove) {
    BB_REQUIRE_GAME(game);
    const Dir dir;
    const fs::path folder = dir.path() / "Bloodborne-Linux";
    const fs::path dump = folder / "game/CUSA03173";
    fs::create_directories(dump / "sce_sys");
    fs::create_symlink(fs::canonical(game) / "eboot.bin", dump / "eboot.bin");
    fs::create_symlink(fs::canonical(game) / "sce_sys/param.sfo", dump / "sce_sys/param.sfo");
    fs::copy_file(BB_LAUNCHER_PATH, folder / "bb-launcher");
    std::ofstream(folder / "bb-run") << std::format("#!/bin/sh\necho \"args=$*\" > {}\n",
                                                    quote((dir.path() / "record.txt").string()));
    fs::permissions(folder / "bb-run", fs::perms::owner_all);
    fs::create_directories(dir.path() / "home");
    const auto launch = [&](const fs::path& at, const std::string& script) {
        std::ofstream(dir.path() / "script.txt") << script;
        return bb::test::run(std::format(
            "cd {} && env -u BB_GAME_DIR -u BB_USER_DIR -u BB_CACHE_DIR -u WAYLAND_DISPLAY "
            "-u DISPLAY LC_ALL=C ASAN_OPTIONS=detect_leaks=0 SDL_VIDEO_DRIVER=offscreen "
            "SDL_RENDER_DRIVER=software BB_LAUNCHER_SCRIPT={} HOME={} timeout 60 {} 2>&1",
            quote(dir.path().string()), quote((dir.path() / "script.txt").string()),
            quote((dir.path() / "home").string()), quote((at / "bb-launcher").string())));
    };
    auto r = launch(folder, "wait 200\nquit\n");
    EXPECT_EQ(r.status, 0) << r.output;
    EXPECT_NE(r.output.find("1 hit(s)"), std::string::npos) << r.output;
    EXPECT_EQ(slurp(folder / "user/config.toml"), "game_dir = \"../game/CUSA03173\"\n");
    EXPECT_TRUE(fs::is_empty(dir.path() / "home"));

    const fs::path moved = dir.path() / "elsewhere/Bloodborne on a stick";
    fs::create_directories(moved.parent_path());
    fs::rename(folder, moved);
    r = launch(moved, "wait 3\npress accept\nwaitgame 20\nwait 3\nquit\n");
    EXPECT_EQ(r.status, 0) << r.output;
    EXPECT_EQ(r.output.find("hit(s)"), std::string::npos) << "no search: the stored folder works";
    const std::string record = slurp(dir.path() / "record.txt");
    EXPECT_NE(record.find("--config " + (fs::canonical(moved) / "user/config.toml").string()),
              std::string::npos)
        << record;
}

// Several hits while the Picture and sound page is open: the page is closed properly first, so
// its choices are saved, and the list of folders replaces it.
TEST(LauncherRunGame, SeveralSearchHitsSaveAnOpenPicturePage) {
    BB_REQUIRE_GAME(game);
    const Dir dir;
    for (const char* name : {"Games/BloodborneA", "Games/BloodborneB"}) {
        const fs::path folder = dir.path() / "home" / name;
        fs::create_directories(folder / "sce_sys");
        fs::create_symlink(fs::canonical(game) / "eboot.bin", folder / "eboot.bin");
        fs::create_symlink(fs::canonical(game) / "sce_sys/param.sfo", folder / "sce_sys/param.sfo");
    }
    for (int i = 0; i < 1500; ++i) {  // keeps the search busy until the page is open
        fs::create_directories(dir.path() / "home/Pad" / std::to_string(i) / "x");
    }
    const auto r = run_script(
        dir, std::string(to_picture) + "press accept\nwait 2\npress down\nwait 2\npress accept\nwait 400\nquit\n");
    EXPECT_EQ(r.status, 0) << r.output;
    EXPECT_NE(r.output.find("2 hit(s)"), std::string::npos) << r.output;
    EXPECT_EQ(slurp(dir.path() / "user/config.toml"), "fullscreen = true\n");
}

// The Picture and sound form with the pad alone: fullscreen, 60 frames and extra sharp are chosen
// in their dropdowns with the d-pad and A (the resolution row has no dropdown in fullscreen), and
// Back writes them to the config file.
TEST(LauncherRun, ThePicturePageSavesItsChoicesOnBack) {
    const Dir dir;
    std::string script = to_picture;
    for (const char* step : {"accept", "down", "accept",              // window -> fullscreen
                             "down", "accept", "down", "accept",      // 30 -> 60
                             "down", "accept", "down", "down", "accept"}) {  // normal -> extra sharp
        script += std::format("press {}\nwait 2\n", step);
    }
    script += std::format("shot {}\npress back\nwait 3\nquit\n", (dir.path() / "shot.png").string());
    const auto r = run_script(dir, script);
    EXPECT_EQ(r.status, 0) << r.output;
    EXPECT_EQ(slurp(dir.path() / "user/config.toml"),
              "fullscreen = true\nfsr = true\nrcas_attenuation = 0\nframe_rate = 60\n");
}

// The rest of the form with the pad alone: the view slider moves with right alone, then the
// sharpest textures, no motion blur and no colour fringes (R11, #668, #746).
TEST(LauncherRun, ThePicturePageSavesTheImageQualityChoices) {
    const Dir dir;
    std::string script = to_picture;
    for (const char* step : {"down", "down", "down", "down",  // mode -> field of view
                             "right", "right", "right", "right", "right", "right", "right", "right",
                             "right",                                          // 43 -> 52 degrees
                             "down", "accept", "down", "down", "down", "down", "accept",  // 16x
                             "down", "accept",                                 // no motion blur
                             "down", "accept"}) {                              // no colour fringes
        script += std::format("press {}\nwait 2\n", step);
    }
    const char* keep = std::getenv("BB_LAUNCHER_TEST_SHOT");  // a shot to look at, if asked for
    script += std::format("shot {}\npress back\nwait 3\nquit\n",
                          keep ? keep : (dir.path() / "shot.png").string());
    const auto r = run_script(dir, script);
    EXPECT_EQ(r.status, 0) << r.output;
    EXPECT_EQ(slurp(dir.path() / "user/config.toml"),
              "anisotropy = 16\nmotion_blur = false\nchromatic_aberration = false\nfov = 52\n");
}

TEST(LauncherRun, ABadScriptEndsWithAMessage) {
    const Dir dir;
    const auto r = run_script(dir, "frobnicate\n");
    EXPECT_EQ(r.status, 1);
    EXPECT_NE(r.output.find("line 1"), std::string::npos) << r.output;
}

}  // namespace

#endif
