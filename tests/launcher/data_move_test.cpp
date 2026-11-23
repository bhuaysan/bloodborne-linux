// Moving the data folder (src/launcher/data_move.hpp): the plan per package, a move that succeeds,
// the refusals, and the moves that fail halfway and must change nothing.
#include "launcher/data_move.hpp"

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <algorithm>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include "common/config.hpp"
#include "common/runtime_dirs.hpp"

namespace {

namespace fs = std::filesystem;
using namespace bb::launcher;

std::string slurp(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void put(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

class DataMoveTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::string pattern = (fs::temp_directory_path() / "bb-move-test-XXXXXX").string();
        ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
        tmp_ = fs::canonical(pattern);
        from_ = tmp_ / "pkg/user";
        // What a data folder holds after some play: config, two profiles, logs and a cache.
        put(from_ / "config.toml", "fullscreen = true\n# a comment\n");
        put(from_ / "profiles/default/savedata/SPRJ0000/sce_sys/param.sfo", "save one");
        put(from_ / "profiles/default/savedata/SPRJ0000/sdimg_0", std::string(70000, 'x'));
        put(from_ / "profiles/other/savedata/SPRJ0001/sdimg_0", "save two");
        put(from_ / "logs/bb-run.log", "log");
        put(from_ / "cache/shaders/a.bin", "cache");
        request_.from = from_;
        request_.to = tmp_ / "data";
        request_.pointer_file = tmp_ / "config/bloodborne-linux/data-dir";
    }
    void TearDown() override {
        fs::permissions(tmp_, fs::perms::owner_all, fs::perm_options::replace);
        fs::remove_all(tmp_);
    }

public:
    const MoveRequest& request() const { return request_; }

protected:
    fs::path tmp_;
    fs::path from_;
    MoveRequest request_;
};

TEST_F(DataMoveTest, ThePlanPerPackage) {
    const fs::path exe = tmp_ / "pkg";
    PlanInput tar;
    tar.exe_dir = exe;
    tar.home = (tmp_ / "home").string();
    const DataPlan writable = plan_data_folder(tar);
    EXPECT_EQ(writable.pointer_file, exe / "data-dir");
    EXPECT_EQ(writable.default_folder, exe / "user");
    EXPECT_TRUE(writable.default_removes_pointer);

    if (::geteuid() != 0) {  // an unwritable program folder: the config folder keeps the choice
        fs::permissions(exe, fs::perms::owner_read | fs::perms::owner_exec);
        fs::permissions(exe / "user", fs::perms::owner_read | fs::perms::owner_exec);
        ::setenv("XDG_CONFIG_HOME", (tmp_ / "xdg").c_str(), 1);
        const DataPlan locked = plan_data_folder(tar);
        ::unsetenv("XDG_CONFIG_HOME");
        EXPECT_EQ(locked.pointer_file, tmp_ / "xdg/bloodborne-linux/data-dir");
        EXPECT_EQ(locked.default_folder, tmp_ / "home/Bloodborne-Linux-data");
        EXPECT_FALSE(locked.default_removes_pointer);
        fs::permissions(exe / "user", fs::perms::owner_all);
        fs::permissions(exe, fs::perms::owner_all);
    }

    PlanInput image = tar;
    image.appimage = true;
    image.appimage_file = tmp_ / "apps/BB.AppImage";
    fs::create_directories(tmp_ / "apps");
    const DataPlan app = plan_data_folder(image);
    EXPECT_EQ(app.default_folder, tmp_ / "apps/Bloodborne-Linux-data");
    EXPECT_FALSE(app.default_removes_pointer);
    EXPECT_EQ(app.pointer_file.filename(), "data-dir");
    EXPECT_NE(app.pointer_file.parent_path(), exe);
}

TEST_F(DataMoveTest, MovesTheFilesWritesThePointerAndRemovesTheOldOnes) {
    const MoveResult r = move_data_folder(request_);
    ASSERT_TRUE(r.ok()) << r.detail;
    EXPECT_TRUE(r.leftovers.empty());
    const fs::path to = request_.to;
    EXPECT_EQ(slurp(to / "config.toml"), "fullscreen = true\n# a comment\n");
    EXPECT_EQ(slurp(to / "profiles/default/savedata/SPRJ0000/sce_sys/param.sfo"), "save one");
    EXPECT_EQ(slurp(to / "profiles/default/savedata/SPRJ0000/sdimg_0"), std::string(70000, 'x'));
    EXPECT_EQ(slurp(to / "profiles/other/savedata/SPRJ0001/sdimg_0"), "save two");
    EXPECT_EQ(slurp(to / "logs/bb-run.log"), "log");
    EXPECT_FALSE(fs::exists(to / "cache"));
    struct stat st {};
    ASSERT_EQ(::stat(to.c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 07777, 0700u);
    EXPECT_EQ(bb::common::read_data_dir_pointer(request_.pointer_file), to);
    EXPECT_FALSE(fs::exists(from_)) << "everything of the old folder went, and the folder with it";
}

TEST_F(DataMoveTest, KeepsTheOldFolderWhenSomethingElseLivesInIt) {
    put(from_ / "game/CUSA03173/eboot.bin", "x");
    ASSERT_TRUE(move_data_folder(request_).ok());
    EXPECT_FALSE(fs::exists(from_ / "config.toml"));
    EXPECT_FALSE(fs::exists(from_ / "profiles"));
    EXPECT_TRUE(fs::exists(from_ / "game/CUSA03173/eboot.bin"));
}

// The AppImage stores game/<dump> and the tar.gz ../game/<dump>: after the move the dump is
// where it was, and the copy names it by its absolute path.
TEST_F(DataMoveTest, ARelativeGameFolderBecomesAbsolute) {
    const fs::path dump = tmp_ / "pkg/game/CUSA03173";
    fs::create_directories(dump);
    put(from_ / "config.toml", "game_dir = \"../game/CUSA03173\"\nfullscreen = true\n");
    request_.forbidden = {dump};
    const MoveResult r = move_data_folder(request_);
    ASSERT_TRUE(r.ok()) << r.detail;
    EXPECT_EQ(slurp(request_.to / "config.toml"),
              "game_dir = \"" + dump.string() + "\"\nfullscreen = true\n");
    EXPECT_EQ(bb::common::read_config(request_.to / "config.toml").config.game_dir, dump);
}

TEST_F(DataMoveTest, AnAbsoluteGameFolderStaysAsItIs) {
    put(from_ / "config.toml", "game_dir = \"/srv/games/CUSA03173\"\n");
    ASSERT_TRUE(move_data_folder(request_).ok());
    EXPECT_EQ(slurp(request_.to / "config.toml"), "game_dir = \"/srv/games/CUSA03173\"\n");
}

TEST_F(DataMoveTest, BackToTheProgramsFolderRemovesThePointerFile) {
    const fs::path exe = tmp_ / "pkg";
    fs::path data = tmp_ / "elsewhere";
    request_.to = data;
    request_.pointer_file = exe / "data-dir";
    ASSERT_TRUE(move_data_folder(request_).ok());
    EXPECT_EQ(bb::common::read_data_dir_pointer(exe / "data-dir"), data);

    MoveRequest back;
    back.from = data;
    back.to = exe / "user";
    back.pointer_file = exe / "data-dir";
    back.remove_pointer = true;
    const MoveResult r = move_data_folder(back);
    ASSERT_TRUE(r.ok()) << r.detail;
    EXPECT_FALSE(fs::exists(exe / "data-dir"));
    EXPECT_EQ(slurp(exe / "user/profiles/other/savedata/SPRJ0001/sdimg_0"), "save two");
    EXPECT_FALSE(fs::exists(data));
}

TEST_F(DataMoveTest, AnEmptyTargetFolderIsUsed) {
    fs::create_directories(request_.to);
    ASSERT_TRUE(move_data_folder(request_).ok());
    EXPECT_EQ(slurp(request_.to / "logs/bb-run.log"), "log");
}

// Nothing changes for a refused target: no pointer, the old folder as it was.
void expect_untouched(const DataMoveTest& t, const MoveRequest& r, const std::string& config) {
    EXPECT_EQ(slurp(t.request().from / "config.toml"), config);
    EXPECT_TRUE(fs::exists(t.request().from / "cache/shaders/a.bin"));
    EXPECT_TRUE(fs::exists(t.request().from / "profiles/other/savedata/SPRJ0001/sdimg_0"));
    EXPECT_FALSE(fs::exists(r.pointer_file));
}

TEST_F(DataMoveTest, RefusesATargetThatIsNotEmpty) {
    put(request_.to / "something", "x");
    EXPECT_EQ(move_data_folder(request_).problem, MoveProblem::not_empty);
    EXPECT_EQ(slurp(request_.to / "something"), "x");
    expect_untouched(*this, request_, "fullscreen = true\n# a comment\n");
}

TEST_F(DataMoveTest, RefusesATargetInsideADump) {
    put(tmp_ / "dump/eboot.bin", "x");
    request_.to = tmp_ / "dump/sub/data";
    EXPECT_EQ(move_data_folder(request_).problem, MoveProblem::inside_game);
    request_.to = tmp_ / "dump";
    EXPECT_EQ(move_data_folder(request_).problem, MoveProblem::is_game);
    EXPECT_FALSE(fs::exists(tmp_ / "dump/sub"));
    expect_untouched(*this, request_, "fullscreen = true\n# a comment\n");
}

TEST_F(DataMoveTest, RefusesTargetsAroundTheGameAndTheOldFolder) {
    fs::create_directories(tmp_ / "games/CUSA03173");
    request_.forbidden = {tmp_ / "games/CUSA03173"};
    request_.to = tmp_ / "games";  // would hold the dump
    EXPECT_EQ(move_data_folder(request_).problem, MoveProblem::overlaps);
    request_.to = from_ / "inside";
    EXPECT_EQ(move_data_folder(request_).problem, MoveProblem::nested);
    request_.to = tmp_ / "pkg";
    EXPECT_EQ(move_data_folder(request_).problem, MoveProblem::nested);
    request_.to = from_;
    EXPECT_EQ(move_data_folder(request_).problem, MoveProblem::same_folder);
    request_.to = "relative/data";
    EXPECT_EQ(move_data_folder(request_).problem, MoveProblem::not_absolute);
    expect_untouched(*this, request_, "fullscreen = true\n# a comment\n");
}

// A copy that fails halfway (the hook stands in for a full disk or an unplugged drive) leaves the
// old folder in use: no pointer, no partial target, and the folders made for it are gone.
TEST_F(DataMoveTest, AFailedCopyChangesNothing) {
    put(request_.pointer_file, tmp_.string() + "/before\n");
    request_.to = tmp_ / "new/deeper/data";
    int copied = 0;
    request_.fail_after_copy = [&](const fs::path&) { return ++copied == 3; };
    const MoveResult r = move_data_folder(request_);
    EXPECT_EQ(r.problem, MoveProblem::copy_failed);
    EXPECT_GE(copied, 3);
    EXPECT_FALSE(fs::exists(tmp_ / "new")) << "the folders made for the move are removed";
    EXPECT_EQ(slurp(request_.pointer_file), tmp_.string() + "/before\n");
    EXPECT_EQ(slurp(from_ / "config.toml"), "fullscreen = true\n# a comment\n");
    EXPECT_EQ(slurp(from_ / "profiles/default/savedata/SPRJ0000/sdimg_0"), std::string(70000, 'x'));
    EXPECT_TRUE(fs::exists(from_ / "cache/shaders/a.bin"));
}

TEST_F(DataMoveTest, AFailedCopyKeepsAnExistingEmptyTargetFolder) {
    fs::create_directories(request_.to);
    request_.fail_after_copy = [](const fs::path&) { return true; };
    EXPECT_EQ(move_data_folder(request_).problem, MoveProblem::copy_failed);
    EXPECT_TRUE(fs::is_directory(request_.to));
    EXPECT_TRUE(fs::is_empty(request_.to));
    EXPECT_FALSE(fs::exists(request_.pointer_file));
}

// A copy that differs from its source (a flipped byte of the same size) is caught by the hash.
TEST_F(DataMoveTest, ACorruptCopyIsCaughtByTheChecksum) {
    request_.fail_after_copy = [](const fs::path& copy) {
        if (copy.filename() == "sdimg_0" && fs::file_size(copy) == 70000) {
            std::fstream f(copy, std::ios::in | std::ios::out | std::ios::binary);
            f.seekp(69999);
            f.put('y');
        }
        return false;
    };
    const MoveResult r = move_data_folder(request_);
    EXPECT_EQ(r.problem, MoveProblem::verify_failed);
    EXPECT_FALSE(fs::exists(request_.to));
    EXPECT_FALSE(fs::exists(request_.pointer_file));
    EXPECT_TRUE(fs::exists(from_ / "profiles/default"));
}

// The pointer can't be written: the copy is removed again and the old folder stays in use.
TEST_F(DataMoveTest, ABrokenPointerUndoesTheCopy) {
    put(tmp_ / "blocker", "a file where the pointer's folder should be");
    request_.pointer_file = tmp_ / "blocker/bloodborne-linux/data-dir";
    EXPECT_EQ(move_data_folder(request_).problem, MoveProblem::pointer_failed);
    EXPECT_FALSE(fs::exists(request_.to));
    EXPECT_EQ(slurp(from_ / "config.toml"), "fullscreen = true\n# a comment\n");
    EXPECT_TRUE(fs::exists(from_ / "cache/shaders/a.bin"));
}

// Old files that can't be removed after the switch are reported, and the new folder stays.
TEST_F(DataMoveTest, OldFilesThatStayAreReportedAfterTheSwitch) {
    if (::geteuid() == 0) {
        GTEST_SKIP() << "root removes anywhere";
    }
    fs::permissions(from_ / "logs", fs::perms::owner_read | fs::perms::owner_exec);
    const MoveResult r = move_data_folder(request_);
    fs::permissions(from_ / "logs", fs::perms::owner_all);
    EXPECT_TRUE(r.ok());
    ASSERT_EQ(r.leftovers.size(), 1u);
    EXPECT_EQ(r.leftovers[0], from_ / "logs");
    EXPECT_EQ(bb::common::read_data_dir_pointer(request_.pointer_file), request_.to);
    EXPECT_EQ(slurp(request_.to / "logs/bb-run.log"), "log");
}

// ---- the plan follows the file the runtime reads first ---------------------------------------

TEST_F(DataMoveTest, AReadOnlyProgramFolderCannotChangeItsDataFolder) {
    if (::geteuid() == 0) {
        GTEST_SKIP() << "root writes anywhere";
    }
    const fs::path exe = tmp_ / "pkg";
    PlanInput tar;
    tar.exe_dir = exe;
    tar.home = (tmp_ / "home").string();
    EXPECT_TRUE(plan_data_folder(tar).changeable);

    // user/ is writable, the folder around it is not: the runtime uses user/ and reads data-dir
    // first, which a Change could not write.
    fs::permissions(exe, fs::perms::owner_read | fs::perms::owner_exec);
    DataPlan plan = plan_data_folder(tar);
    EXPECT_FALSE(plan.changeable);
    EXPECT_EQ(plan.pointer_file, exe / "data-dir");

    // data-dir exists in a read-only folder (moved there after a Change): still that file.
    fs::permissions(exe, fs::perms::owner_all);
    put(exe / "data-dir", "/srv/somewhere\n");
    fs::permissions(exe / "user", fs::perms::owner_read | fs::perms::owner_exec);
    fs::permissions(exe, fs::perms::owner_read | fs::perms::owner_exec);
    plan = plan_data_folder(tar);
    EXPECT_FALSE(plan.changeable);
    EXPECT_EQ(plan.pointer_file, exe / "data-dir") << "not the config folder's file";
    fs::permissions(exe, fs::perms::owner_all);
    fs::permissions(exe / "user", fs::perms::owner_all);
}

TEST_F(DataMoveTest, NoDefaultFolderOrNoPointerFileIsNoPlace) {
    MoveRequest r = request_;
    r.to = fs::path();
    EXPECT_EQ(move_data_folder(r).problem, MoveProblem::no_place);
    r = request_;
    r.pointer_file = fs::path();
    EXPECT_EQ(move_data_folder(r).problem, MoveProblem::no_place);
    EXPECT_FALSE(fs::exists(request_.to));
    expect_untouched(*this, request_, "fullscreen = true\n# a comment\n");
}

// ---- a target that holds only game/ and cache/ (Back to default of an AppImage) ----------------

TEST_F(DataMoveTest, ATargetWithOnlyGameAndCacheIsUsedAndTheyStay) {
    put(request_.to / "game/CUSA03173/eboot.bin", "dump");
    put(request_.to / "cache/system/m.bin", "mesa");
    ASSERT_TRUE(move_data_folder(request_).ok());
    EXPECT_EQ(slurp(request_.to / "game/CUSA03173/eboot.bin"), "dump");
    EXPECT_EQ(slurp(request_.to / "cache/system/m.bin"), "mesa");
    EXPECT_EQ(slurp(request_.to / "logs/bb-run.log"), "log");
}

TEST_F(DataMoveTest, AFailedCopyIntoAFolderWithGameKeepsGameAndCache) {
    put(request_.to / "game/CUSA03173/eboot.bin", "dump");
    put(request_.to / "cache/system/m.bin", "mesa");
    request_.fail_after_copy = [](const fs::path&) { return true; };
    EXPECT_EQ(move_data_folder(request_).problem, MoveProblem::copy_failed);
    EXPECT_EQ(slurp(request_.to / "game/CUSA03173/eboot.bin"), "dump");
    EXPECT_EQ(slurp(request_.to / "cache/system/m.bin"), "mesa");
    EXPECT_FALSE(fs::exists(request_.to / "config.toml"));
    EXPECT_FALSE(fs::exists(request_.to / "profiles"));
}

TEST_F(DataMoveTest, AStrayFileBesideGameStillRefusesTheTarget) {
    put(request_.to / "game/x", "x");
    put(request_.to / "notes.txt", "x");
    EXPECT_EQ(move_data_folder(request_).problem, MoveProblem::not_empty);
}

// ---- a folder shared with the player's own files -------------------------------------------------

TEST_F(DataMoveTest, ASharedOldFolderKeepsItsOwnFilesAndOnlyLosesOurCache) {
    put(from_ / "notes.txt", "mine");
    put(from_ / "cache/other/keep.bin", "not ours");
    put(from_ / "cache/system/m.bin", "mesa");
    put(from_ / "cache/pipeline/p.bin", "pipe");
    put(from_ / "cache/game-check.txt", "eboot x\n");
    const MoveResult r = move_data_folder(request_);
    ASSERT_TRUE(r.ok()) << r.detail;
    EXPECT_FALSE(fs::exists(from_ / "cache/game-check.txt"));
    EXPECT_EQ(slurp(from_ / "notes.txt"), "mine");
    EXPECT_EQ(slurp(from_ / "cache/other/keep.bin"), "not ours");
    EXPECT_FALSE(fs::exists(from_ / "cache/system"));
    EXPECT_FALSE(fs::exists(from_ / "cache/pipeline"));
    EXPECT_TRUE(fs::exists(from_ / "cache/shaders/a.bin")) << "not made by the runtime: left alone";
    ASSERT_EQ(r.kept.size(), 1u);
    EXPECT_EQ(r.kept[0], from_ / "notes.txt");
    EXPECT_FALSE(fs::exists(from_ / "config.toml"));
}

TEST_F(DataMoveTest, RemoveOldCacheTakesAllOfCacheOnlyInARuntimeFolder) {
    put(from_ / "cache/other/x", "x");
    remove_old_cache(from_);
    EXPECT_FALSE(fs::exists(from_ / "cache"));
    put(from_ / "notes.txt", "mine");
    put(from_ / "cache/other/x", "x");
    put(from_ / "cache/system/y", "y");
    remove_old_cache(from_);
    EXPECT_TRUE(fs::exists(from_ / "cache/other/x"));
    EXPECT_FALSE(fs::exists(from_ / "cache/system"));
}

// ---- what a copy can't take ----------------------------------------------------------------------

TEST_F(DataMoveTest, ASymlinkOrAFifoInProfilesIsRefusedAndTheTargetRemoved) {
    request_.to = tmp_ / "new/data";
    fs::create_symlink("/srv/elsewhere", from_ / "profiles/default/link");
    EXPECT_EQ(move_data_folder(request_).problem, MoveProblem::copy_failed);
    EXPECT_FALSE(fs::exists(tmp_ / "new")) << "the folder made for the move is removed";
    fs::remove(from_ / "profiles/default/link");
    ASSERT_EQ(::mkfifo((from_ / "profiles/default/pipe").c_str(), 0600), 0);
    EXPECT_EQ(move_data_folder(request_).problem, MoveProblem::copy_failed);
    EXPECT_FALSE(fs::exists(tmp_ / "new"));
    EXPECT_FALSE(fs::exists(request_.pointer_file));
    expect_untouched(*this, request_, "fullscreen = true\n# a comment\n");
}

// ---- durability and aliases, seen from outside ---------------------------------------------------

// The child half of the next two tests: they start this binary again (under strace, or in a
// mount namespace) with BB_MOVE_INNER naming the fixture's folder.
TEST(DataMoveInner, Run) {
    const char* base = std::getenv("BB_MOVE_INNER");
    const char* mode = std::getenv("BB_MOVE_MODE");
    if (base == nullptr || mode == nullptr) {
        GTEST_SKIP() << "only as the child of another test";
    }
    const fs::path tmp = base;
    MoveRequest r;
    r.from = tmp / "pkg/user";
    r.pointer_file = tmp / "config/bloodborne-linux/data-dir";
    if (std::string(mode) == "flush") {
        r.to = tmp / "data";
        EXPECT_TRUE(move_data_folder(r).ok());
        return;
    }
    r.to = tmp / (std::string(mode) == "bindinner" ? "alias/data" : "aliasparent");
    EXPECT_EQ(move_data_folder(r).problem, MoveProblem::nested);
    EXPECT_FALSE(fs::exists(tmp / "pkg/user/cache/data"));
    EXPECT_TRUE(fs::exists(tmp / "pkg/user/config.toml"));
}

std::string exe_path() { return fs::read_symlink("/proc/self/exe").string(); }

// Every copied file is flushed, and the pointer is switched, before the first old file is deleted.
TEST_F(DataMoveTest, TheCopyIsFlushedBeforeTheOldFilesGo) {
    if (std::system("strace -e trace=none true > /dev/null 2>&1") != 0) {
        GTEST_SKIP() << "strace is not usable here";
    }
    const fs::path log = tmp_ / "strace.log";
    const std::string cmd = "ASAN_OPTIONS=detect_leaks=0 BB_MOVE_INNER=" + tmp_.string() +
                            " BB_MOVE_MODE=flush strace -f -y -qq "
                            "-e trace=fsync,syncfs,unlinkat,rename,renameat,renameat2 -o " +
                            log.string() + " " + exe_path() +
                            " --gtest_filter=DataMoveInner.Run > /dev/null 2>&1";
    ASSERT_EQ(std::system(cmd.c_str()), 0) << "the move failed in the child";
    std::ifstream in(log);
    std::string line;
    int file_fsyncs = 0;
    bool pointer_renamed = false;
    bool deleted = false;
    while (std::getline(in, line)) {
        if (line.find("unlinkat") != std::string::npos) {
            deleted = true;  // nothing is deleted before the old files go (no undo here)
            break;
        }
        if (line.find("rename") != std::string::npos &&
            line.find("data-dir") != std::string::npos) {
            pointer_renamed = true;
        }
        // strace -y prints the path of the descriptor: fsync(3</...>); folders end in a slash-less
        // name too, so a file is told by being one of the copied names.
        if (line.find("fsync(") != std::string::npos &&
            (line.find("config.toml") != std::string::npos ||
             line.find("param.sfo") != std::string::npos ||
             line.find("sdimg_0") != std::string::npos ||
             line.find("bb-run.log") != std::string::npos)) {
            ++file_fsyncs;
        }
    }
    EXPECT_TRUE(deleted);
    EXPECT_GE(file_fsyncs, 5) << "each of the 5 copied files is flushed first";
    EXPECT_TRUE(pointer_renamed) << "the pointer is switched before the old files go";
    EXPECT_TRUE(fs::exists(tmp_ / "data/logs/bb-run.log"));
}

// A second mount of the old cache (or of the folder around it) leads into the old folder under
// another name: the textual check can't see it, the device and inode can.
TEST_F(DataMoveTest, ATargetReachedThroughAnotherMountIsRefused) {
    // Some CI sandboxes allow the namespace but refuse the bind mount: probe the mount itself.
    const std::string probe = tmp_.string() + "/probe";
    fs::create_directories(probe + "/x");
    fs::create_directories(probe + "/y");
    if (std::system(("unshare -Urm sh -c 'mount --bind " + probe + "/x " + probe +
                     "/y' > /dev/null 2>&1")
                        .c_str()) != 0) {
        GTEST_SKIP() << "no user and mount namespaces with bind mounts here";
    }
    fs::create_directories(tmp_ / "alias");
    fs::create_directories(tmp_ / "aliasparent");
    for (const char* mode : {"bindinner", "bindouter"}) {
        const std::string t = tmp_.string();
        const std::string cmd =
            "unshare -Urm sh -c 'mount --bind " + t + "/pkg/user/cache " + t + "/alias && " +
            "mount --bind " + t + "/pkg " + t + "/aliasparent && ASAN_OPTIONS=detect_leaks=0 " +
            "BB_MOVE_INNER=" + t + " BB_MOVE_MODE=" + mode + " " + exe_path() +
            " --gtest_filter=DataMoveInner.Run' > /dev/null 2>&1";
        EXPECT_EQ(std::system(cmd.c_str()), 0) << mode;
    }
    EXPECT_TRUE(fs::exists(from_ / "config.toml"));
    EXPECT_FALSE(fs::exists(from_ / "cache/data"));
}

}  // namespace
