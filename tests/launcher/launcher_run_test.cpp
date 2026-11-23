// Runs bb-launcher on SDL's offscreen video driver with a script of steps (see
// src/launcher/logic.hpp) and looks at what it writes.
#ifdef BB_LAUNCHER_PATH

#include <stdlib.h>
#include <unistd.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

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

bb::test::Result run_script(const Dir& dir, const std::string& script,
                            const std::string& more_env = "") {
    std::ofstream(dir.path() / "script.txt") << script;
    const std::string d = dir.path().string();
    return bb::test::run(std::format(
        "cd {} && env -u BB_GAME_DIR -u BB_CACHE_DIR -u WAYLAND_DISPLAY -u DISPLAY LC_ALL=C ASAN_OPTIONS=detect_leaks=0 "
        "SDL_VIDEO_DRIVER=offscreen SDL_RENDER_DRIVER=software SDL_AUDIO_DRIVER=dummy "
        "{}BB_LAUNCHER_SCRIPT={} HOME={} BB_USER_DIR={} timeout 60 {} 2>&1",
        quote(d), more_env, quote(d + "/script.txt"), quote(d + "/home"),
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

std::string slurp_file(const fs::path& file);

// Runs bb-launcher as an AppImage's runtime starts it (APPIMAGE at `appimage`, APPDIR the folder
// of the launcher), with HOME and TMPDIR below dir, and no data folder variables.
bb::test::Result run_appimage_script(const Dir& dir, const std::string& script,
                                     const fs::path& appimage) {
    fs::create_directories(dir.path() / "home");
    fs::create_directories(dir.path() / "tmp");
    std::ofstream(dir.path() / "script.txt") << script;
    const std::string d = dir.path().string();
    return bb::test::run(std::format(
        "cd {} && env -u BB_GAME_DIR -u BB_CACHE_DIR -u BB_USER_DIR -u XDG_CONFIG_HOME -u XDG_CACHE_HOME "
        "-u WAYLAND_DISPLAY -u DISPLAY LC_ALL=C ASAN_OPTIONS=detect_leaks=0 SDL_VIDEO_DRIVER=offscreen "
        "SDL_RENDER_DRIVER=software BB_LAUNCHER_SCRIPT={} APPIMAGE={} APPDIR={} HOME={} TMPDIR={} "
        "timeout 60 {} 2>&1",
        quote(d), quote(d + "/script.txt"), quote(appimage.string()),
        quote(fs::path(BB_LAUNCHER_PATH).parent_path().string()), quote(d + "/home"),
        quote(d + "/tmp"), quote(BB_LAUNCHER_PATH)));
}

// Every file below `root`, relative to it.
std::vector<std::string> files_below(const fs::path& root) {
    std::vector<std::string> files;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_directory()) {
            files.push_back(fs::relative(entry.path(), root).string());
        }
    }
    return files;
}

// The first start of an AppImage: without a data folder the launcher asks, the script takes the
// proposed one, and only the pointer file lands outside it. The first run's cache folder is gone
// at the end, and nothing else is written to HOME.
TEST(LauncherRun, AnAppImageWithoutADataFolderAsksAndWritesThePointer) {
    const Dir dir;
    fs::create_directories(dir.path() / "apps");
    const auto r = run_appimage_script(
        dir,
        std::format("wait 3\nshot {0}\ndatadefault\nwait 5\nshot {1}\nquit\n",
                    (dir.path() / "ask.png").string(), (dir.path() / "after.png").string()),
        dir.path() / "apps/BB.AppImage");
    EXPECT_EQ(r.status, 0) << r.output;
    EXPECT_TRUE(is_png(dir.path() / "ask.png"));
    EXPECT_NE(slurp_file(dir.path() / "ask.png"), slurp_file(dir.path() / "after.png"));
    EXPECT_EQ(slurp_file(dir.path() / "home/.config/bloodborne-linux/data-dir"),
              (fs::canonical(dir.path() / "apps") / "Bloodborne-Linux-data").string() + "\n");
    EXPECT_TRUE(fs::is_directory(dir.path() / "apps/Bloodborne-Linux-data"));
    EXPECT_EQ(files_below(dir.path() / "home"),
              std::vector<std::string>{".config/bloodborne-linux/data-dir"});
    EXPECT_TRUE(fs::is_empty(dir.path() / "tmp")) << "the first-start cache folder is removed";
}

// The AppImage file lies in the dump: the data folder must not go there, the proposal moves to HOME.
TEST(LauncherRun, AnAppImageKeptInTheDumpGetsItsDataFolderInHome) {
    const Dir dir;
    const fs::path dump = dir.path() / "CUSA03173";
    fs::create_directories(dump);
    std::ofstream(dump / "eboot.bin") << "x";
    const auto r = run_appimage_script(dir, "wait 3\ndatadefault\nwait 5\nquit\n",
                                       dump / "BB.AppImage");
    EXPECT_EQ(r.status, 0) << r.output;
    EXPECT_EQ(slurp_file(dir.path() / "home/.config/bloodborne-linux/data-dir"),
              (fs::canonical(dir.path() / "home") / "Bloodborne-Linux-data").string() + "\n");
    EXPECT_EQ(files_below(dump), std::vector<std::string>{"eboot.bin"});
}

// "Choose..." on a folder inside the dump is refused, and the screen stays: no pointer.
TEST(LauncherRun, ADataFolderInsideTheDumpIsRefused) {
    const Dir dir;
    const fs::path dump = dir.path() / "CUSA03173";
    fs::create_directories(dump / "sub");
    std::ofstream(dump / "eboot.bin") << "x";
    const auto r = run_appimage_script(dir,
                                       std::format("wait 3\ndatafolder {}\nwait 3\nquit\n",
                                                   (dump / "sub").string()),
                                       dir.path() / "BB.AppImage");
    EXPECT_EQ(r.status, 0) << r.output;
    EXPECT_NE(r.output.find("data folder " + (dump / "sub").string()), std::string::npos) << r.output;
    EXPECT_FALSE(fs::exists(dir.path() / "home/.config/bloodborne-linux/data-dir"));
    EXPECT_EQ(files_below(dump), std::vector<std::string>{"eboot.bin"});
}

// A data folder that holds the dump (not in game/) would make every Play fail in bb-run: the
// launcher refuses the game folder before it writes anything.
TEST(LauncherRun, AGameFolderAroundTheDataFolderIsRefused) {
    const Dir dir;
    const fs::path games = dir.path() / "Games";
    fs::create_directories(games / "CUSA03173");
    const auto r = run_appimage_script(
        dir,
        std::format("wait 3\ndatafolder {0}\nwait 5\ndrop {1}\nwait 3\nquit\n", games.string(),
                    (games / "CUSA03173").string()),
        dir.path() / "BB.AppImage");
    EXPECT_EQ(r.status, 0) << r.output;
    EXPECT_NE(r.output.find("bb-launcher: game folder " + (games / "CUSA03173").string()),
              std::string::npos) << r.output;
    EXPECT_NE(r.output.find("overlaps"), std::string::npos) << r.output;
    EXPECT_FALSE(fs::exists(games / "config.toml")) << "no game_dir was written";
}

// A data-dir file that names a folder that is gone (an unmounted USB stick): the first-start
// screen comes up with the reason, the folder is not made again and the file stays until the
// player chooses.
TEST(LauncherRun, AMissingDataFolderBringsBackTheFirstStartScreen) {
    const Dir dir;
    fs::create_directories(dir.path() / "home/.config/bloodborne-linux");
    const std::string gone = (dir.path() / "usb/Bloodborne-Linux-data").string();
    std::ofstream(dir.path() / "home/.config/bloodborne-linux/data-dir") << gone << "\n";
    const auto r = run_appimage_script(
        dir, std::format("wait 3\nshot {}\nquit\n", (dir.path() / "lost.png").string()),
        dir.path() / "BB.AppImage");
    EXPECT_EQ(r.status, 0) << r.output;
    EXPECT_NE(r.output.find("the data folder " + gone + " does not exist"), std::string::npos)
        << r.output;
    EXPECT_FALSE(fs::exists(dir.path() / "usb"));
    EXPECT_EQ(slurp_file(dir.path() / "home/.config/bloodborne-linux/data-dir"), gone + "\n");
    // The screen is the first-start screen with the reason, not the main screen with a notice.
    EXPECT_EQ(r.output.find("No place for the game's data"), std::string::npos) << r.output;
    EXPECT_EQ(r.output.find("This folder can't be written"), std::string::npos) << r.output;
    EXPECT_TRUE(is_png(dir.path() / "lost.png"));
}

std::string slurp(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string slurp_file(const fs::path& file) { return slurp(file); }

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

// A user folder that can't be written: a dropped dump keeps that reason in the status line
// instead of the error of writing a config file without a path.
TEST(LauncherRunGame, ADropWithoutAUserFolderKeepsTheReason) {
    BB_REQUIRE_GAME(game);
    const Dir dir;
    fs::create_directories(dir.path() / "user");
    fs::permissions(dir.path() / "user", fs::perms::owner_read | fs::perms::owner_exec);
    const auto r = run_script(dir, std::format("wait 3\ndrop {}\nwait 3\nquit\n", game.string()));
    fs::permissions(dir.path() / "user", fs::perms::owner_all);
    EXPECT_EQ(r.status, 0) << r.output;
    EXPECT_NE(r.output.find("can't be written"), std::string::npos) << r.output;
    EXPECT_EQ(r.output.find("filesystem error"), std::string::npos) << r.output;
    EXPECT_TRUE(fs::is_empty(dir.path() / "user"));
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
    // The search step hands the result over once fullscreen is chosen on the open page.
    const auto r = run_script(dir, std::string(to_picture) +
                                       "press accept\nwait 2\npress down\nwait 2\npress accept\n"
                                       "wait 3\nsearch\nwait 3\nquit\n");
    EXPECT_EQ(r.status, 0) << r.output;
    EXPECT_NE(r.output.find("2 hit(s)"), std::string::npos) << r.output;
    EXPECT_EQ(slurp(dir.path() / "user/config.toml"), "fullscreen = true\n");
}

// A controller on the machine that runs the tests (someone playing next to them) holds the d-pad
// down from the start: the script's presses alone move the focus, so Window -> Fullscreen is
// still chosen and saved (#788). SDL sees only the virtual pad (vendor and product 0), so the
// run is the same whatever is plugged into the machine.
TEST(LauncherRun, AControllerNextToTheRunDoesNotMoveTheScript) {
    const Dir dir;
    const auto r = run_script(dir,
                              std::string("otherpad down\nwait 3\n") + to_picture +
                                  "press accept\nwait 2\npress down\nwait 2\npress accept\n"
                                  "wait 3\npress back\nwait 3\nquit\n",
                              "SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x0000/0x0000 ");
    EXPECT_EQ(r.status, 0) << r.output;
    EXPECT_EQ(slurp(dir.path() / "user/config.toml"), "fullscreen = true\n") << r.output;
}

// The Picture and sound form with the pad alone: fullscreen, 60 frames and VSync on are chosen in
// their dropdowns with the d-pad and A (the resolution row has no dropdown in fullscreen and is
// skipped), the sharpness slider goes to its end with right alone, and Back writes them to the
// config file. The monitor and graphics card rows are passed over: their lists depend on the
// machine.
TEST(LauncherRun, ThePicturePageSavesItsChoicesOnBack) {
    const Dir dir;
    std::string script = to_picture;
    for (const char* step : {"accept", "down", "accept",                  // window -> fullscreen
                             "down", "down", "accept", "down", "accept",  // 30 -> 60
                             "down", "accept", "down", "accept",          // VSync on
                             "down", "down"}) {                           // to the sharpness
        script += std::format("press {}\nwait 2\n", step);
    }
    for (int i = 0; i < 12; ++i) {  // off -> 10, the sharpest; it stops there
        script += "press right\nwait 2\n";
    }
    script += std::format("shot {}\npress back\nwait 3\nquit\n", (dir.path() / "shot.png").string());
    const auto r = run_script(dir, script);
    EXPECT_EQ(r.status, 0) << r.output;
    EXPECT_EQ(slurp(dir.path() / "user/config.toml"),
              "fullscreen = true\nfsr = true\nrcas_attenuation = 0\nframe_rate = 60\n"
              "present_mode = \"fifo\"\n");
}

// The rest of the form with the pad alone: the sliders move with left and right alone (the
// field of view by a degree, the effects by 5 %), then the sharpest textures (R11, #668, #746,
// #749).
TEST(LauncherRun, ThePicturePageSavesTheImageQualityChoices) {
    const Dir dir;
    std::string script = to_picture;
    std::vector<const char*> steps(7, "down");  // mode -> field of view
    steps.insert(steps.end(), 9, "right");      // 43 -> 52 degrees
    for (const char* step : {"down", "accept", "down", "down", "down", "down", "accept"}) {
        steps.push_back(step);  // 16x
    }
    steps.push_back("down");
    steps.insert(steps.end(), 13, "left");  // motion blur 100 -> 35 %
    steps.push_back("down");
    steps.insert(steps.end(), 21, "left");  // colour fringes 100 -> off, and it stays there
    for (const char* step : steps) {
        script += std::format("press {}\nwait 2\n", step);
    }
    const char* keep = std::getenv("BB_LAUNCHER_TEST_SHOT");  // a shot to look at, if asked for
    script += std::format("shot {}\npress back\nwait 3\nquit\n",
                          keep ? keep : (dir.path() / "shot.png").string());
    const auto r = run_script(dir, script);
    EXPECT_EQ(r.status, 0) << r.output;
    EXPECT_EQ(slurp(dir.path() / "user/config.toml"),
              "anisotropy = 16\nmotion_blur = true\nmotion_blur_strength = 0.35\n"
              "chromatic_aberration = false\nfov = 52\n");
}

// The sound output, the last row (#750): its dropdown lists System default, then the devices SDL
// lists, here the one of SDL's dummy driver (named in SDL's SDL_audio.c, DEFAULT_PLAYBACK_DEVNAME).
// The first device goes into audio_device. A row added above it changes the count of downs.
TEST(LauncherRun, ThePicturePageSavesTheSoundOutput) {
    const Dir dir;
    std::string script = to_picture;
    std::vector<const char*> steps(11, "down");  // mode -> sound output
    for (const char* step : {"accept", "down", "accept"}) {
        steps.push_back(step);
    }
    for (const char* step : steps) {
        script += std::format("press {}\nwait 2\n", step);
    }
    const char* keep = std::getenv("BB_LAUNCHER_TEST_SHOT");  // a shot to look at, if asked for
    script += std::format("shot {}\npress back\nwait 3\nquit\n",
                          keep ? keep : (dir.path() / "shot.png").string());
    const auto r = run_script(dir, script);
    EXPECT_EQ(r.status, 0) << r.output;
    EXPECT_EQ(slurp(dir.path() / "user/config.toml"),
              "audio_device = \"System audio playback device\"\n");
}

// The Advanced page: Settings, its third entry, the load hold slider from the game's 12 s down to
// 0 with left alone (#704).
TEST(LauncherRun, TheAdvancedPageSavesTheLoadHold) {
    const Dir dir;
    std::string script = "wait 3\npress down\nwait 2\npress accept\nwait 3\npress down\nwait 2\n"
                         "press down\nwait 2\npress accept\nwait 3\n";
    for (int i = 0; i < 13; ++i) {
        script += "press left\nwait 2\n";
    }
    const char* keep = std::getenv("BB_LAUNCHER_TEST_SHOT");  // a shot to look at, if asked for
    script += std::format("shot {}\npress back\nwait 3\nquit\n",
                          keep ? keep : (dir.path() / "shot.png").string());
    const auto r = run_script(dir, script);
    EXPECT_EQ(r.status, 0) << r.output;
    EXPECT_EQ(slurp(dir.path() / "user/config.toml"), "load_hold = 0\n");
}

// Runs a copy of the launcher as a tar.gz package starts it (no APPIMAGE, no BB_USER_DIR), with
// HOME at dir/home.
bb::test::Result run_tar_script(const Dir& dir, const fs::path& launcher, const std::string& script) {
    fs::create_directories(dir.path() / "home");
    fs::create_directories(dir.path() / "tmp");
    std::ofstream(dir.path() / "script.txt") << script;
    const std::string d = dir.path().string();
    return bb::test::run(std::format(
        "cd {} && env -u BB_GAME_DIR -u BB_CACHE_DIR -u BB_USER_DIR -u XDG_CONFIG_HOME -u APPIMAGE "
        "-u APPDIR -u WAYLAND_DISPLAY -u DISPLAY LC_ALL=C ASAN_OPTIONS=detect_leaks=0 "
        "SDL_VIDEO_DRIVER=offscreen SDL_RENDER_DRIVER=software BB_LAUNCHER_SCRIPT={} HOME={} "
        "TMPDIR={} timeout 60 {} 2>&1",
        quote(d), quote(d + "/script.txt"), quote(d + "/home"), quote(d + "/tmp"),
        quote(launcher.string())));
}

// A copy of bb-launcher in `folder`, as the tar.gz has it.
fs::path install_launcher(const fs::path& folder) {
    fs::create_directories(folder);
    fs::copy_file(BB_LAUNCHER_PATH, folder / "bb-launcher");
    return folder / "bb-launcher";
}

// A data folder with a config, a save, a log and a cache, as play leaves it.
void fill_data_folder(const fs::path& folder) {
    fs::create_directories(folder / "profiles/default/savedata/SPRJ0000");
    fs::create_directories(folder / "cache/shaders");
    fs::create_directories(folder / "logs");
    std::ofstream(folder / "config.toml") << "fullscreen = true\n";
    std::ofstream(folder / "profiles/default/savedata/SPRJ0000/sdimg_0") << "the save";
    std::ofstream(folder / "logs/bb-run.log") << "log";
    std::ofstream(folder / "cache/shaders/a.bin") << "cache";
}

// An AppImage moves its data folder (the script plays Change and Back to default): the files
// arrive checked, the pointer follows, the old folder and the cache are gone, and the next start
// finds the saves where the pointer says.
TEST(LauncherRun, AnAppImageMovesItsDataFolderAndTheNextStartFindsIt) {
    const Dir dir;
    const fs::path apps = dir.path() / "apps";
    fs::create_directories(apps);
    fill_data_folder(dir.path() / "data-a");
    fs::create_directories(dir.path() / "home/.config/bloodborne-linux");
    const fs::path pointer = dir.path() / "home/.config/bloodborne-linux/data-dir";
    std::ofstream(pointer) << (dir.path() / "data-a").string() << "\n";
    const fs::path save = "profiles/default/savedata/SPRJ0000/sdimg_0";

    auto r = run_appimage_script(
        dir, std::format("wait 3\nmovedata {0}/data-b\nwait 4\nshot {0}/b.png\nquit\n",
                         dir.path().string()),
        apps / "BB.AppImage");
    EXPECT_EQ(r.status, 0) << r.output;
    EXPECT_NE(r.output.find("data folder moved to " + (dir.path() / "data-b").string()),
              std::string::npos) << r.output;
    EXPECT_EQ(slurp_file(pointer), (dir.path() / "data-b").string() + "\n");
    EXPECT_EQ(slurp_file(dir.path() / "data-b" / save), "the save");
    EXPECT_EQ(slurp_file(dir.path() / "data-b/config.toml"), "fullscreen = true\n");
    EXPECT_FALSE(fs::exists(dir.path() / "data-b/cache/shaders")) << "the old cache is left behind";
    EXPECT_FALSE(fs::exists(dir.path() / "data-a"));
    EXPECT_TRUE(is_png(dir.path() / "b.png"));

    // Next start: no first-start question, the save is where the pointer says.
    r = run_appimage_script(dir, "wait 3\nquit\n", apps / "BB.AppImage");
    EXPECT_EQ(r.status, 0) << r.output;
    EXPECT_EQ(r.output.find("no data folder"), std::string::npos) << r.output;
    EXPECT_EQ(slurp_file(dir.path() / "data-b" / save), "the save");

    // Back to default: the folder next to the AppImage, and the pointer names it.
    r = run_appimage_script(dir, "wait 3\nmovedefault\nwait 4\nquit\n", apps / "BB.AppImage");
    EXPECT_EQ(r.status, 0) << r.output;
    const fs::path home_default = fs::canonical(apps) / "Bloodborne-Linux-data";
    EXPECT_EQ(slurp_file(pointer), home_default.string() + "\n");
    EXPECT_EQ(slurp_file(home_default / save), "the save");
    EXPECT_FALSE(fs::exists(dir.path() / "data-b"));
}

// A move into a folder that holds something is refused with the reason, and nothing changes.
TEST(LauncherRun, AMoveIntoAFullFolderIsRefusedAndNothingChanges) {
    const Dir dir;
    fill_data_folder(dir.path() / "data-a");
    fs::create_directories(dir.path() / "full");
    std::ofstream(dir.path() / "full/mine.txt") << "x";
    fs::create_directories(dir.path() / "home/.config/bloodborne-linux");
    const fs::path pointer = dir.path() / "home/.config/bloodborne-linux/data-dir";
    std::ofstream(pointer) << (dir.path() / "data-a").string() << "\n";
    const auto r = run_appimage_script(
        dir, std::format("wait 3\nmovedata {}/full\nwait 4\nquit\n", dir.path().string()),
        dir.path() / "apps/BB.AppImage");
    EXPECT_EQ(r.status, 0) << r.output;
    EXPECT_NE(r.output.find("bb-launcher: data folder " + (dir.path() / "full").string() + ": "),
              std::string::npos) << r.output;
    EXPECT_EQ(r.output.find("data folder moved"), std::string::npos) << r.output;
    EXPECT_EQ(slurp_file(pointer), (dir.path() / "data-a").string() + "\n");
    EXPECT_EQ(slurp_file(dir.path() / "data-a/profiles/default/savedata/SPRJ0000/sdimg_0"),
              "the save");
    EXPECT_EQ(files_below(dir.path() / "full"), std::vector<std::string>{"mine.txt"});
}

// The tar.gz keeps the choice in `data-dir` next to the program (the config folder stays
// untouched), and Back to default removes that file and moves the data back to user/.
TEST(LauncherRun, ATarGzMovesItsDataFolderThroughAFileNextToTheProgram) {
    const Dir dir;
    const fs::path pkg = dir.path() / "pkg";
    const fs::path launcher = install_launcher(pkg);
    fill_data_folder(pkg / "user");
    const fs::path save = "profiles/default/savedata/SPRJ0000/sdimg_0";

    auto r = run_tar_script(
        dir, launcher,
        std::format("wait 3\nmovedata {}/moved\nwait 4\nquit\n", dir.path().string()));
    EXPECT_EQ(r.status, 0) << r.output;
    EXPECT_EQ(slurp_file(pkg / "data-dir"), (dir.path() / "moved").string() + "\n");
    EXPECT_EQ(slurp_file(dir.path() / "moved" / save), "the save");
    EXPECT_FALSE(fs::exists(pkg / "user"));
    EXPECT_FALSE(fs::exists(dir.path() / "home/.config")) << "the config folder is not touched";

    r = run_tar_script(dir, launcher, "wait 3\nquit\n");  // the next start uses the file
    EXPECT_EQ(r.status, 0) << r.output;
    EXPECT_FALSE(fs::exists(pkg / "user"));

    r = run_tar_script(dir, launcher, "wait 3\nmovedefault\nwait 4\nquit\n");
    EXPECT_EQ(r.status, 0) << r.output;
    EXPECT_FALSE(fs::exists(pkg / "data-dir"));
    EXPECT_EQ(slurp_file(pkg / "user" / save), "the save");
    EXPECT_FALSE(fs::exists(dir.path() / "moved"));
    EXPECT_FALSE(fs::exists(dir.path() / "home/.config"));
}

// A tar.gz in a folder that can't be written asks for a data folder like an AppImage, keeps the
// choice in the config folder and runs from it on the next start.
TEST(LauncherRun, ATarGzInAReadOnlyFolderAsksForADataFolderAndRunsFromIt) {
    if (::geteuid() == 0) {
        GTEST_SKIP() << "root writes anywhere";
    }
    const Dir dir;
    const fs::path pkg = dir.path() / "ro";
    const fs::path launcher = install_launcher(pkg);
    fs::permissions(pkg, fs::perms::owner_read | fs::perms::owner_exec);
    const fs::path pointer = dir.path() / "home/.config/bloodborne-linux/data-dir";
    const fs::path chosen = fs::canonical(dir.path()) / "home/Bloodborne-Linux-data";

    auto r = run_tar_script(
        dir, launcher,
        std::format("wait 3\nshot {}/ask.png\ndatadefault\nwait 5\nquit\n", dir.path().string()));
    EXPECT_EQ(r.status, 0) << r.output;
    EXPECT_TRUE(is_png(dir.path() / "ask.png"));
    EXPECT_EQ(slurp_file(pointer), chosen.string() + "\n");
    EXPECT_TRUE(fs::is_directory(chosen));
    EXPECT_FALSE(fs::exists(pkg / "user"));
    EXPECT_FALSE(fs::exists(pkg / "data-dir"));

    // The next start has its folder: no question. A move changes the config folder's file.
    fill_data_folder(chosen);
    r = run_tar_script(dir, launcher,
                       std::format("wait 3\nmovedata {}/moved\nwait 4\nquit\n", dir.path().string()));
    fs::permissions(pkg, fs::perms::owner_all);
    EXPECT_EQ(r.status, 0) << r.output;
    EXPECT_EQ(r.output.find("no data folder"), std::string::npos) << r.output;
    EXPECT_EQ(slurp_file(pointer), (dir.path() / "moved").string() + "\n");
    EXPECT_EQ(slurp_file(dir.path() / "moved/profiles/default/savedata/SPRJ0000/sdimg_0"),
              "the save");
    EXPECT_FALSE(fs::exists(pkg / "data-dir"));
}

// The data folder of a tar.gz sat on a USB stick that is gone: `data-dir` next to the program names
// it. The first-start screen's choice replaces that file (the runtime reads it first), so the next
// start runs from the new folder, and the pointer of an AppImage on the same machine is untouched.
TEST(LauncherRun, ATarGzWhoseDataFolderIsGoneChoosesAnotherAndRunsFromIt) {
    const Dir dir;
    const fs::path pkg = dir.path() / "pkg";
    const fs::path launcher = install_launcher(pkg);
    std::ofstream(pkg / "data-dir") << "/srv/unplugged/Bloodborne-Linux-data\n";
    const fs::path appimage_pointer = dir.path() / "home/.config/bloodborne-linux/data-dir";
    fs::create_directories(appimage_pointer.parent_path());
    std::ofstream(appimage_pointer) << "/srv/appimage-data\n";
    const fs::path chosen = fs::canonical(dir.path()) / "home/Bloodborne-Linux-data";

    auto r = run_tar_script(
        dir, launcher,
        std::format("wait 3\nshot {}/ask.png\ndatadefault\nwait 5\nquit\n", dir.path().string()));
    EXPECT_EQ(r.status, 0) << r.output;
    EXPECT_TRUE(is_png(dir.path() / "ask.png"));
    EXPECT_EQ(slurp_file(pkg / "data-dir"), chosen.string() + "\n");
    EXPECT_TRUE(fs::is_directory(chosen));
    EXPECT_EQ(slurp_file(appimage_pointer), "/srv/appimage-data\n");

    r = run_tar_script(dir, launcher, "wait 3\nquit\n");  // no question again
    EXPECT_EQ(r.status, 0) << r.output;
    EXPECT_EQ(r.output.find("does not exist"), std::string::npos) << r.output;
    EXPECT_EQ(slurp_file(pkg / "data-dir"), chosen.string() + "\n");
}

// A to B and back to A: at the end the sweep of the folders the data left must not touch A, which
// is in use again (its cache and the folder itself stay).
TEST(LauncherRun, MovingAwayAndBackKeepsTheCurrentFoldersCacheAtTheEnd) {
    const Dir dir;
    const fs::path pkg = dir.path() / "pkg";
    const fs::path launcher = install_launcher(pkg);
    fill_data_folder(pkg / "user");
    const std::string d = dir.path().string();
    const auto r = run_tar_script(
        dir, launcher,
        std::format("wait 3\nmovedata {0}/b\nwait 4\nmovedata {0}/pkg/user\nwait 4\nquit\n", d));
    EXPECT_EQ(r.status, 0) << r.output;
    EXPECT_EQ(slurp_file(pkg / "data-dir"), (fs::canonical(pkg) / "user").string() + "\n");
    EXPECT_EQ(slurp_file(pkg / "user/profiles/default/savedata/SPRJ0000/sdimg_0"), "the save");
    EXPECT_TRUE(fs::is_directory(pkg / "user/cache")) << "the cache in use stays";
    EXPECT_FALSE(fs::exists(dir.path() / "b"));
}

TEST(LauncherRun, ABadScriptEndsWithAMessage) {
    const Dir dir;
    const auto r = run_script(dir, "frobnicate\n");
    EXPECT_EQ(r.status, 1);
    EXPECT_NE(r.output.find("line 1"), std::string::npos) << r.output;
}

}  // namespace

#endif
