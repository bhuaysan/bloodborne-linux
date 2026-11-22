#include "common/runtime_dirs.hpp"

#include <array>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include <gtest/gtest.h>

namespace {

namespace fs = std::filesystem;
using bb::common::RuntimeDirs;

constexpr std::array<const char*, 11> env_keys = {
    "APPIMAGE", "APPDIR",
    "HOME",         "XDG_DATA_HOME",         "XDG_CACHE_HOME",
    "XDG_CONFIG_HOME", "XDG_STATE_HOME",     "BB_USER_DIR",
    "BB_CACHE_DIR", "MESA_SHADER_CACHE_DIR", "__GL_SHADER_DISK_CACHE_PATH"};

// The variable's value, empty when unset.
std::string env(const char* key) {
    const char* value = std::getenv(key);
    return value != nullptr ? value : "";
}

// Every test runs with BB_USER_DIR pointing into a fresh temporary directory, every other
// variable above unset, and restores the environment afterwards. Nothing is written outside that
// directory.
class RuntimeDirsTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::string templ = (fs::temp_directory_path() / "bb-dirs-test-XXXXXX").string();
        ASSERT_NE(::mkdtemp(templ.data()), nullptr);
        tmp_ = fs::canonical(templ);
        for (const char* key : env_keys) {
            const char* value = std::getenv(key);
            saved_[key] = value ? std::optional<std::string>(value) : std::nullopt;
            ::unsetenv(key);
        }
        set("BB_USER_DIR", (tmp_ / "home/user").string());
    }
    void TearDown() override {
        for (const auto& [key, value] : saved_) {
            if (value) {
                ::setenv(key.c_str(), value->c_str(), 1);
            } else {
                ::unsetenv(key.c_str());
            }
        }
        if (!tmp_.empty()) {
            fs::remove_all(tmp_);
        }
    }

    static void set(const char* key, const std::string& value) { ::setenv(key, value.c_str(), 1); }

    static RuntimeDirs make(std::string_view profile = bb::common::default_profile,
                            const std::vector<fs::path>& forbidden = {}) {
        return RuntimeDirs::from_environment(profile, forbidden);
    }

    static unsigned mode(const fs::path& path) {
        struct stat st {};
        EXPECT_EQ(::stat(path.c_str(), &st), 0) << path;
        return st.st_mode & 07777;
    }

    fs::path tmp_;
    std::map<std::string, std::optional<std::string>> saved_;
};

TEST_F(RuntimeDirsTest, EverythingBelowTheUserDir) {
    RuntimeDirs dirs = make();
    fs::path user = tmp_ / "home/user";
    EXPECT_EQ(dirs.profile(), "default");
    EXPECT_EQ(dirs.root(), user);
    EXPECT_EQ(dirs.data(), user);
    EXPECT_EQ(dirs.config(), user);
    EXPECT_EQ(dirs.state(), user);
    EXPECT_EQ(dirs.cache(), user / "cache");
    EXPECT_EQ(dirs.profile_data(), user / "profiles/default");
    EXPECT_EQ(dirs.profile_cache(), user / "cache/profiles/default");
    EXPECT_EQ(dirs.logs(), user / "logs");
    EXPECT_FALSE(fs::exists(tmp_ / "home")) << "nothing is created before first use";
}

TEST_F(RuntimeDirsTest, IgnoresXdgAndHome) {
    for (const char* key : {"HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_CONFIG_HOME",
                            "XDG_STATE_HOME"}) {
        set(key, (tmp_ / key).string());
    }
    set("BB_USER_DIR", (tmp_ / "u/").string());
    RuntimeDirs dirs = make("test-1_A");
    EXPECT_EQ(dirs.root(), tmp_ / "u");
    EXPECT_EQ(dirs.cache(), tmp_ / "u/cache");
    EXPECT_EQ(dirs.profile_data(), tmp_ / "u/profiles/test-1_A");
}

TEST_F(RuntimeDirsTest, CacheDirOverridesOnlyTheCache) {
    set("BB_CACHE_DIR", (tmp_ / "warm").string());
    RuntimeDirs dirs = make();
    EXPECT_EQ(dirs.cache(), tmp_ / "warm");
    EXPECT_EQ(dirs.root(), tmp_ / "home/user");
    dirs.ensure(dirs.cache() / "pipeline");
    EXPECT_TRUE(fs::is_directory(tmp_ / "warm/pipeline"));
}

TEST_F(RuntimeDirsTest, WithoutUserDirTheFolderIsNextToTheBinary) {
    std::error_code ec;
    EXPECT_EQ(bb::common::executable_dir(), fs::read_symlink("/proc/self/exe", ec).parent_path());
    // Empty values count as unset.
    for (const std::string value : {""}) {
        set("BB_USER_DIR", value);
        set("BB_CACHE_DIR", value);
        const fs::path expected = fs::weakly_canonical(bb::common::executable_dir() / "user");
        if (expected.string().starts_with(fs::weakly_canonical(bb::common::source_dir()).string())) {
            // A test binary in the build tree: refused, and the message says what to set.
            try {
                make();
                ADD_FAILURE() << "a folder inside the checkout was accepted";
            } catch (const std::runtime_error& e) {
                EXPECT_NE(std::string(e.what()).find("BB_USER_DIR"), std::string::npos) << e.what();
            }
        } else {
            EXPECT_EQ(make().root(), expected);
            EXPECT_EQ(make().cache(), expected / "cache");
        }
    }
}

TEST_F(RuntimeDirsTest, ProfileNames) {
    using bb::common::valid_profile_name;
    EXPECT_TRUE(valid_profile_name("default"));
    EXPECT_TRUE(valid_profile_name("a"));
    EXPECT_TRUE(valid_profile_name(std::string(32, 'z')));
    EXPECT_TRUE(valid_profile_name("AZaz09_-"));
    const std::vector<std::string> bad_names = {
        "", "..", ".", "a/b", "a b", "a.b", "\xc3\xa4", std::string(33, 'z'),
        std::string("a\0b", 3)};
    for (const std::string& bad : bad_names) {
        EXPECT_FALSE(valid_profile_name(bad)) << bad;
        EXPECT_THROW(make(bad), std::invalid_argument) << bad;
    }
}

TEST_F(RuntimeDirsTest, RefusesDirectoriesInsideTheGameDirectory) {
    fs::path game = tmp_ / "game";
    fs::create_directory(game);
    set("BB_USER_DIR", (game / "user").string());
    EXPECT_THROW(make("default", {game}), std::runtime_error);
    // Also through a symlink and with a trailing slash on the game directory.
    fs::create_directory_symlink(game, tmp_ / "link");
    set("BB_USER_DIR", (tmp_ / "link/x").string());
    EXPECT_THROW(make("default", {tmp_ / "game/"}), std::runtime_error);
    // And the other way round: the game inside the user folder.
    set("BB_USER_DIR", (tmp_ / "outer").string());
    EXPECT_THROW(make("default", {tmp_ / "outer/game"}), std::runtime_error);
    // The cache alone counts too.
    set("BB_USER_DIR", (tmp_ / "elsewhere").string());
    set("BB_CACHE_DIR", (game / "cache").string());
    EXPECT_THROW(make("default", {game}), std::runtime_error);
    ::unsetenv("BB_CACHE_DIR");
    // A sibling with a common prefix is fine, and the game may sit next to user/.
    set("BB_USER_DIR", (tmp_ / "game-data").string());
    EXPECT_NO_THROW(make("default", {game}));
    set("BB_USER_DIR", (tmp_ / "folder/user").string());
    EXPECT_NO_THROW(make("default", {tmp_ / "folder/game"}));
    EXPECT_FALSE(fs::exists(tmp_ / "game-data"));
    EXPECT_TRUE(fs::is_empty(game));
}

TEST_F(RuntimeDirsTest, RefusesRelativeVariables) {
    // Ignoring them would silently put the folder somewhere else.
    set("BB_USER_DIR", "relative/user");
    EXPECT_THROW(make(), std::runtime_error);
    set("BB_USER_DIR", (tmp_ / "u").string());
    set("BB_CACHE_DIR", "relative/cache");
    EXPECT_THROW(make(), std::runtime_error);
}

TEST_F(RuntimeDirsTest, RefusesACacheSymlinkIntoTheGameDirectory) {
    // shadPS4's pipeline cache and the drivers create their folders without ensure(), so the
    // overlap check has to see through a user/cache symlink.
    fs::path game = tmp_ / "game";
    fs::create_directory(game);
    fs::create_directories(tmp_ / "home/user");
    fs::create_directory_symlink(game, tmp_ / "home/user/cache");
    EXPECT_THROW(make("default", {game}), std::runtime_error);
    EXPECT_TRUE(fs::is_empty(game));
}

TEST_F(RuntimeDirsTest, RefusesDirectoriesInsideTheSourceCheckout) {
    // Only computes paths: nothing is created in the checkout.
    set("BB_USER_DIR", (bb::common::source_dir() / "build").string());
    EXPECT_THROW(make(), std::runtime_error);
    set("BB_USER_DIR", (tmp_ / "u").string());
    set("BB_CACHE_DIR", (bb::common::source_dir() / "build").string());
    EXPECT_THROW(make(), std::runtime_error);
}

TEST_F(RuntimeDirsTest, EnsureCreatesWithMode0700) {
    RuntimeDirs dirs = make("p1");
    fs::path save = dirs.profile_data() / "savedata/0";
    EXPECT_EQ(dirs.ensure(save), save);
    EXPECT_TRUE(fs::is_directory(save));
    for (fs::path p = save; p != tmp_; p = p.parent_path()) {
        EXPECT_EQ(mode(p), 0700u) << p;
    }
    EXPECT_EQ(dirs.ensure(save), save) << "a second call is a no-op";
    EXPECT_EQ(dirs.ensure(dirs.logs() / "."), dirs.logs());
    EXPECT_TRUE(fs::is_directory(dirs.logs()));
}

TEST_F(RuntimeDirsTest, EnsureKeepsModeOfExistingDirectories) {
    set("BB_USER_DIR", tmp_.string());
    RuntimeDirs dirs = make();
    EXPECT_EQ(mode(tmp_), 0700u);  // mkdtemp
    ::chmod(tmp_.c_str(), 0755);
    dirs.ensure(dirs.logs());
    EXPECT_EQ(mode(tmp_), 0755u);
    EXPECT_EQ(mode(dirs.logs()), 0700u);
}

TEST_F(RuntimeDirsTest, EnsureRefusesOtherDirectories) {
    RuntimeDirs dirs = make();
    EXPECT_THROW(dirs.ensure(tmp_ / "elsewhere"), std::logic_error);
    EXPECT_THROW(dirs.ensure(dirs.data() / "../escape"), std::logic_error);
    EXPECT_THROW(dirs.ensure("relative"), std::logic_error);
    EXPECT_FALSE(fs::exists(tmp_ / "elsewhere"));
}

TEST_F(RuntimeDirsTest, EnsureFailsOnAFileInTheWay) {
    RuntimeDirs dirs = make();
    dirs.ensure(dirs.data());
    std::ofstream(dirs.data() / "profiles") << "x";
    EXPECT_THROW(dirs.ensure(dirs.profile_data()), fs::filesystem_error);
}

TEST_F(RuntimeDirsTest, EnsureRefusesSymlinksBelowTheBase) {
    fs::path game = tmp_ / "game";
    fs::create_directory(game);
    RuntimeDirs dirs = make("default", {game});
    dirs.ensure(dirs.data());
    fs::create_directory_symlink(game, dirs.data() / "profiles");
    EXPECT_THROW(dirs.ensure(dirs.profile_data()), fs::filesystem_error);
    EXPECT_TRUE(fs::is_empty(game));
    // A dangling symlink is refused too, and so is the base itself as a symlink.
    fs::create_symlink(tmp_ / "missing", dirs.data() / "dangling");
    EXPECT_THROW(dirs.ensure(dirs.data() / "dangling/x"), fs::filesystem_error);
    EXPECT_FALSE(fs::exists(tmp_ / "missing"));
    set("BB_USER_DIR", (tmp_ / "second").string());
    RuntimeDirs second = make("default", {game});
    fs::create_directory_symlink(game, second.root());
    EXPECT_THROW(second.ensure(second.logs()), fs::filesystem_error);
    EXPECT_TRUE(fs::is_empty(game));
}

TEST_F(RuntimeDirsTest, LibraryCachesGoBelowTheCache) {
    RuntimeDirs dirs = make();
    bb::common::redirect_library_caches(dirs);
    const fs::path system = dirs.cache() / "system";
    EXPECT_EQ(env("XDG_CACHE_HOME"), system.string());
    EXPECT_EQ(env("MESA_SHADER_CACHE_DIR"), system.string());
    EXPECT_EQ(env("__GL_SHADER_DISK_CACHE_PATH"), (system / "nvidia").string());
    EXPECT_TRUE(fs::is_directory(system / "nvidia"));
    EXPECT_EQ(std::getenv("XDG_CONFIG_HOME"), nullptr) << "only caches are redirected";
    EXPECT_EQ(std::getenv("XDG_DATA_HOME"), nullptr);
}

TEST_F(RuntimeDirsTest, LibraryCachesAreRedirectedEvenWhenTheFolderIsReadOnly) {
    if (::geteuid() == 0) {
        GTEST_SKIP() << "root writes anywhere";
    }
    fs::create_directories(tmp_ / "ro");
    ::chmod((tmp_ / "ro").c_str(), 0500);
    set("BB_USER_DIR", (tmp_ / "ro/user").string());
    RuntimeDirs dirs = make();
    EXPECT_THROW(dirs.ensure(dirs.root()), fs::filesystem_error);
    bb::common::redirect_library_caches(dirs);
    EXPECT_EQ(env("XDG_CACHE_HOME"), (dirs.cache() / "system").string());
    ::chmod((tmp_ / "ro").c_str(), 0700);
}


// ---- The AppImage's data folder (BB_USER_DIR, then the pointer file, never user/ in the mount) ----

class AppImageDirsTest : public RuntimeDirsTest {
protected:
    void SetUp() override {
        RuntimeDirsTest::SetUp();
        ::unsetenv("BB_USER_DIR");
        set("APPIMAGE", (tmp_ / "Bloodborne.AppImage").string());
        // The tests run from the build directory, which stands in for the AppImage's mount.
        set("APPDIR", bb::common::executable_dir().string());
        set("XDG_CONFIG_HOME", (tmp_ / "config").string());
        pointer_ = tmp_ / "config/bloodborne-linux/data-dir";
    }
    void write_pointer(const std::string& text) {
        fs::create_directories(pointer_.parent_path());
        std::ofstream(pointer_) << text;
    }
    fs::path pointer_;
};

TEST_F(AppImageDirsTest, RunningFromAppImageNeedsAnAbsoluteAppImageVariable) {
    EXPECT_TRUE(bb::common::running_from_appimage());
    set("APPIMAGE", "relative.AppImage");
    EXPECT_FALSE(bb::common::running_from_appimage());
    set("APPIMAGE", "");
    EXPECT_FALSE(bb::common::running_from_appimage());
    ::unsetenv("APPIMAGE");
    EXPECT_FALSE(bb::common::running_from_appimage());
}

// The runtime exports APPIMAGE and APPDIR to every child: a program that doesn't run from the
// mount (a tar.gz package started from another AppImage) must not take the AppImage's rules.
TEST_F(AppImageDirsTest, AnAppImageVariableWithoutTheExecutableInAppDirIsNotAnAppImage) {
    EXPECT_TRUE(bb::common::running_from_appimage());
    set("APPDIR", (tmp_ / "other-mount").string());  // an AppImage that started this program
    EXPECT_FALSE(bb::common::running_from_appimage());
    set("APPDIR", "relative");
    EXPECT_FALSE(bb::common::running_from_appimage());
    set("APPDIR", "");
    EXPECT_FALSE(bb::common::running_from_appimage());
    ::unsetenv("APPDIR");
    EXPECT_FALSE(bb::common::running_from_appimage());
    // Not an AppImage: the pointer file is not read, and BB_USER_DIR-less user/ is the root.
    set("APPDIR", (tmp_ / "other-mount").string());
    fs::create_directories(tmp_ / "data");
    write_pointer((tmp_ / "data").string() + "\n");
    try {
        make();
    } catch (const bb::common::NoDataFolder&) {
        FAIL() << "not an AppImage: no data folder question";
    } catch (const std::runtime_error&) {
        // the build directory lies in the source tree: the overlap message is not the pointer's
    }
}

TEST(AppImagePredicate, NeedsAbsolutePathsAndTheExecutableInsideAppDir) {
    using bb::common::is_appimage_environment;
    EXPECT_TRUE(is_appimage_environment("/srv/BB.AppImage", "/srv/mnt/appdir",
                                        "/srv/mnt/appdir/usr/bin"));
    EXPECT_TRUE(is_appimage_environment("/srv/BB.AppImage", "/srv/mnt/appdir/",
                                        "/srv/mnt/appdir"));
    EXPECT_FALSE(is_appimage_environment("/srv/BB.AppImage", "/srv/mnt/appdir",
                                         "/srv/mnt/appdir2"));
    EXPECT_FALSE(is_appimage_environment("/srv/BB.AppImage", "/srv/mnt/appdir", "/srv/tar/bin"));
    EXPECT_FALSE(is_appimage_environment("/srv/BB.AppImage", "", "/srv/mnt/appdir"));
    EXPECT_FALSE(is_appimage_environment("/srv/BB.AppImage", "appdir", "/srv/mnt/appdir"));
    EXPECT_FALSE(is_appimage_environment("", "/srv/mnt/appdir", "/srv/mnt/appdir"));
    EXPECT_FALSE(is_appimage_environment("BB.AppImage", "/srv/mnt/appdir", "/srv/mnt/appdir"));
}

TEST_F(AppImageDirsTest, NoPointerFileThrowsNoDataFolder) {
    try {
        make();
        FAIL() << "expected NoDataFolder";
    } catch (const bb::common::NoDataFolder& e) {
        EXPECT_NE(std::string(e.what()).find("bb-launcher"), std::string::npos);
        EXPECT_NE(std::string(e.what()).find("BB_USER_DIR"), std::string::npos);
    }
}

TEST_F(AppImageDirsTest, PointerFileGivesTheRootWithTheFinalNewlineRemoved) {
    fs::create_directories(tmp_ / "data");
    write_pointer((tmp_ / "data").string() + "\n");
    RuntimeDirs dirs = make();
    EXPECT_EQ(dirs.root(), tmp_ / "data");
    EXPECT_EQ(dirs.cache(), tmp_ / "data/cache");
}

// Only the one newline of the writer goes: a folder name may end in a space.
TEST_F(AppImageDirsTest, PointerFileKeepsTrailingSpacesOfTheFolderName) {
    fs::create_directories(tmp_ / "data ");
    write_pointer((tmp_ / "data ").string() + "\n");
    EXPECT_EQ(bb::common::read_data_dir_pointer(), tmp_ / "data ");
    EXPECT_EQ(make().root(), tmp_ / "data ");
    write_pointer((tmp_ / "data").string());  // no newline at all is fine too
    EXPECT_EQ(bb::common::read_data_dir_pointer(), tmp_ / "data");
}

TEST_F(AppImageDirsTest, BbUserDirBeatsThePointerFile) {
    fs::create_directories(tmp_ / "data");
    write_pointer((tmp_ / "data").string() + "\n");
    set("BB_USER_DIR", (tmp_ / "other").string());
    EXPECT_EQ(make().root(), tmp_ / "other");
}

// A pointer file that is empty, relative or has several lines sends the player back to the
// first-start screen (NoDataFolder), and the message names the file.
TEST_F(AppImageDirsTest, AnUnusablePointerFileIsNoDataFolderNamingTheFile) {
    for (const char* text : {"data\n", "\n", "", "/srv/a\n/srv/b\n", "\n\n"}) {
        write_pointer(text);
        EXPECT_THROW(bb::common::read_data_dir_pointer(), std::runtime_error) << text;
        try {
            make();
            FAIL() << "expected NoDataFolder for '" << text << "'";
        } catch (const bb::common::NoDataFolder& e) {
            EXPECT_EQ(e.reason(), bb::common::NoDataFolder::Reason::pointer_unusable);
            EXPECT_NE(std::string(e.what()).find(pointer_.string()), std::string::npos) << e.what();
            EXPECT_NE(std::string(e.what()).find("bb-launcher"), std::string::npos) << e.what();
        }
    }
}

// An unmounted USB stick: the folder is gone, and it is not made again behind the player's back.
TEST_F(AppImageDirsTest, AMissingDataFolderIsNoDataFolderAndStaysMissing) {
    const fs::path gone = tmp_ / "usb/Bloodborne-Linux-data";
    write_pointer(gone.string() + "\n");
    try {
        make();
        FAIL() << "expected NoDataFolder";
    } catch (const bb::common::NoDataFolder& e) {
        EXPECT_EQ(e.reason(), bb::common::NoDataFolder::Reason::folder_missing);
        EXPECT_EQ(e.folder(), gone);
        EXPECT_NE(std::string(e.what()).find(gone.string()), std::string::npos) << e.what();
        EXPECT_NE(std::string(e.what()).find("bb-launcher"), std::string::npos) << e.what();
    }
    EXPECT_FALSE(fs::exists(tmp_ / "usb"));
    fs::create_directories(tmp_ / "usb");
    std::ofstream(gone) << "a file, not a folder";
    EXPECT_THROW(make(), bb::common::NoDataFolder);
}

TEST_F(AppImageDirsTest, AnUnwritableDataFolderIsNoDataFolder) {
    if (::geteuid() == 0) {
        GTEST_SKIP() << "root writes anywhere";
    }
    const fs::path ro = tmp_ / "ro-data";
    fs::create_directories(ro);
    ::chmod(ro.c_str(), 0500);
    write_pointer(ro.string() + "\n");
    try {
        make();
        ::chmod(ro.c_str(), 0700);
        FAIL() << "expected NoDataFolder";
    } catch (const bb::common::NoDataFolder& e) {
        ::chmod(ro.c_str(), 0700);
        EXPECT_EQ(e.reason(), bb::common::NoDataFolder::Reason::folder_unwritable);
        EXPECT_EQ(e.folder(), ro);
    }
}

// A file that can't even be looked up (no access to its folder) is an error that names the file;
// it is not "no data folder chosen yet", which would lead the player to overwrite a good choice.
TEST_F(AppImageDirsTest, ALookupErrorOfThePointerFileNamesTheFile) {
    if (::geteuid() == 0) {
        GTEST_SKIP() << "root reads anywhere";
    }
    write_pointer((tmp_ / "data").string() + "\n");
    ::chmod(pointer_.parent_path().c_str(), 0000);
    try {
        bb::common::read_data_dir_pointer();
        ::chmod(pointer_.parent_path().c_str(), 0700);
        FAIL() << "expected an error";
    } catch (const bb::common::NoDataFolder&) {
        ::chmod(pointer_.parent_path().c_str(), 0700);
        FAIL() << "read_data_dir_pointer never throws NoDataFolder";
    } catch (const std::runtime_error& e) {
        ::chmod(pointer_.parent_path().c_str(), 0700);
        EXPECT_NE(std::string(e.what()).find(pointer_.string()), std::string::npos) << e.what();
    }
}

TEST_F(AppImageDirsTest, WithoutAppImageThePointerFileIsIgnored) {
    ::unsetenv("APPIMAGE");
    fs::create_directories(tmp_ / "data");
    write_pointer((tmp_ / "data").string() + "\n");
    set("BB_USER_DIR", (tmp_ / "home/user").string());
    EXPECT_EQ(make().root(), tmp_ / "home/user");
    ::unsetenv("BB_USER_DIR");
    // user/ next to the test executable, as before. In a build directory inside the checkout that
    // overlaps the source tree and throws: the message then names user/, never the pointed folder.
    const fs::path expected = fs::weakly_canonical(bb::common::executable_dir() / "user");
    try {
        EXPECT_EQ(make().root(), expected);
    } catch (const bb::common::NoDataFolder&) {
        FAIL() << "the pointer file was read";
    } catch (const std::runtime_error& e) {
        const std::string what = e.what();
        EXPECT_NE(what.find(expected.string()), std::string::npos) << what;
        EXPECT_EQ(what.find((tmp_ / "data").string()), std::string::npos) << what;
        EXPECT_EQ(what.find("data-dir"), std::string::npos) << what;
    }
}

TEST_F(AppImageDirsTest, PointerRoundTripCreatesTheFolderPrivately) {
    const fs::path folder = tmp_ / "chosen";
    bb::common::write_data_dir_pointer(folder);
    EXPECT_EQ(bb::common::data_dir_pointer_path(), pointer_);
    EXPECT_EQ(mode(pointer_.parent_path()), 0700u);
    EXPECT_EQ(bb::common::read_data_dir_pointer(), folder);
    EXPECT_FALSE(fs::exists(pointer_.string() + ".tmp"));
    bb::common::write_data_dir_pointer(tmp_ / "second");  // replaces the old content
    EXPECT_EQ(bb::common::read_data_dir_pointer(), tmp_ / "second");
    EXPECT_THROW(bb::common::write_data_dir_pointer("relative"), std::invalid_argument);
}

// The file holds exactly the path and one newline, and a folder name with a newline is refused:
// the one-line file could not give it back.
TEST_F(AppImageDirsTest, PointerFileHoldsOneLineAndRefusesANewlineInTheName) {
    bb::common::write_data_dir_pointer(tmp_ / "a b ");
    std::ifstream in(pointer_, std::ios::binary);
    const std::string content((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    EXPECT_EQ(content, (tmp_ / "a b ").string() + "\n");
    EXPECT_EQ(mode(pointer_), 0600u);
    EXPECT_THROW(bb::common::write_data_dir_pointer(tmp_ / "bad\nname"), std::invalid_argument);
    EXPECT_EQ(bb::common::read_data_dir_pointer(), tmp_ / "a b ");  // the old content stays
}

TEST_F(AppImageDirsTest, ConfigHomeFallsBackToHomeDotConfig) {
    ::unsetenv("XDG_CONFIG_HOME");
    set("HOME", (tmp_ / "h").string());
    EXPECT_EQ(bb::common::data_dir_pointer_path(), tmp_ / "h/.config/bloodborne-linux/data-dir");
    set("XDG_CONFIG_HOME", "relative");  // ignored: not absolute
    EXPECT_EQ(bb::common::data_dir_pointer_path(), tmp_ / "h/.config/bloodborne-linux/data-dir");
    ::unsetenv("HOME");
    ::unsetenv("XDG_CONFIG_HOME");
    EXPECT_THROW(bb::common::data_dir_pointer_path(), std::runtime_error);
}

TEST_F(AppImageDirsTest, OverlapChecksApplyToThePointedFolderButAllowGameInside) {
    fs::create_directories(tmp_ / "data");
    write_pointer((tmp_ / "data").string() + "\n");
    EXPECT_THROW(make(bb::common::default_profile, {tmp_ / "data"}), std::runtime_error);
    EXPECT_THROW(make(bb::common::default_profile, {tmp_ / "data/profiles"}), std::runtime_error);
    EXPECT_THROW(make(bb::common::default_profile, {tmp_}), std::runtime_error);
    EXPECT_NO_THROW(make(bb::common::default_profile, {tmp_ / "data/game/dump"}));
    EXPECT_NO_THROW(bb::common::check_data_folder(tmp_ / "data", {tmp_ / "data/game"}));
    EXPECT_THROW(bb::common::check_data_folder(tmp_ / "data", {tmp_ / "data/other"}),
                 std::runtime_error);
}

// ---- The tar.gz's data folder (BB_USER_DIR, `data-dir` next to the program, the config folder's
// pointer only when the program's folder can't hold user/, then user/) ---------------------------

class TarGzDirsTest : public RuntimeDirsTest {
protected:
    void SetUp() override {
        RuntimeDirsTest::SetUp();
        ::unsetenv("BB_USER_DIR");
        set("XDG_CONFIG_HOME", (tmp_ / "config").string());
        exe_ = tmp_ / "pkg";
        fs::create_directories(exe_);
        config_pointer_ = tmp_ / "config/bloodborne-linux/data-dir";
    }
    void TearDown() override {
        ::chmod(exe_.c_str(), 0700);
        RuntimeDirsTest::TearDown();
    }
    RuntimeDirs make_tar(bool appimage = false) const {
        return RuntimeDirs::from_environment("default", {}, RuntimeDirs::Package{exe_, appimage});
    }
    void write_to(const fs::path& file, const std::string& text) const {
        fs::create_directories(file.parent_path());
        std::ofstream(file) << text;
    }
    // The program's folder can't be written (copied to /usr/bin, say).
    bool lock_program_folder() const {
        ::chmod(exe_.c_str(), 0500);
        return ::geteuid() != 0;
    }
    fs::path exe_;
    fs::path config_pointer_;
};

TEST_F(TarGzDirsTest, AWritableFolderUsesUserNextToTheProgram) {
    EXPECT_EQ(make_tar().root(), exe_ / "user");
    EXPECT_EQ(make_tar().cache(), exe_ / "user/cache");
}

TEST_F(TarGzDirsTest, TheFileNextToTheProgramGivesTheFolder) {
    fs::create_directories(tmp_ / "saves");
    write_to(bb::common::program_data_dir_path(exe_), (tmp_ / "saves").string() + "\n");
    EXPECT_EQ(make_tar().root(), tmp_ / "saves");
}

// The config folder's pointer belongs to an AppImage: a tar.gz that can write its own folder
// never reads it.
TEST_F(TarGzDirsTest, AWritableFolderIgnoresTheConfigFoldersPointer) {
    fs::create_directories(tmp_ / "appimage-data");
    write_to(config_pointer_, (tmp_ / "appimage-data").string() + "\n");
    EXPECT_EQ(make_tar().root(), exe_ / "user");
}

TEST_F(TarGzDirsTest, TheFileNextToTheProgramBeatsTheConfigFoldersPointer) {
    fs::create_directories(tmp_ / "one");
    fs::create_directories(tmp_ / "two");
    write_to(config_pointer_, (tmp_ / "one").string() + "\n");
    write_to(bb::common::program_data_dir_path(exe_), (tmp_ / "two").string() + "\n");
    EXPECT_EQ(make_tar().root(), tmp_ / "two");
    ::chmod(exe_.c_str(), 0500);  // also when the program's folder is read-only
    EXPECT_EQ(make_tar().root(), tmp_ / "two");
}

TEST_F(TarGzDirsTest, BbUserDirBeatsBothFiles) {
    fs::create_directories(tmp_ / "one");
    write_to(config_pointer_, (tmp_ / "one").string() + "\n");
    write_to(bb::common::program_data_dir_path(exe_), (tmp_ / "one").string() + "\n");
    set("BB_USER_DIR", (tmp_ / "dev").string());
    EXPECT_EQ(make_tar().root(), tmp_ / "dev");
}

TEST_F(TarGzDirsTest, AFolderThatCantBeWrittenUsesTheConfigFoldersPointer) {
    fs::create_directories(tmp_ / "chosen");
    write_to(config_pointer_, (tmp_ / "chosen").string() + "\n");
    if (!lock_program_folder()) {
        GTEST_SKIP() << "root writes anywhere";
    }
    EXPECT_EQ(make_tar().root(), tmp_ / "chosen");
}

TEST_F(TarGzDirsTest, AFolderThatCantBeWrittenAndHasNoChoiceAsksTheLauncher) {
    if (!lock_program_folder()) {
        GTEST_SKIP() << "root writes anywhere";
    }
    try {
        make_tar();
        FAIL() << "no data folder was accepted";
    } catch (const bb::common::NoDataFolder& e) {
        EXPECT_EQ(e.reason(), bb::common::NoDataFolder::Reason::not_chosen);
    }
}

// An administrator who made user/ for the player keeps the old behaviour.
TEST_F(TarGzDirsTest, AWritableUserFolderInAProtectedFolderIsUsed) {
    fs::create_directories(exe_ / "user");
    if (!lock_program_folder()) {
        GTEST_SKIP() << "root writes anywhere";
    }
    EXPECT_EQ(make_tar().root(), exe_ / "user");
}

TEST_F(TarGzDirsTest, AChosenFolderThatIsGoneOrBrokenIsNoDataFolder) {
    write_to(bb::common::program_data_dir_path(exe_), (tmp_ / "usb").string() + "\n");
    try {
        make_tar();
        FAIL() << "a missing folder was accepted";
    } catch (const bb::common::NoDataFolder& e) {
        EXPECT_EQ(e.reason(), bb::common::NoDataFolder::Reason::folder_missing);
        EXPECT_EQ(e.folder(), tmp_ / "usb");
    }
    EXPECT_FALSE(fs::exists(tmp_ / "usb"));
    write_to(bb::common::program_data_dir_path(exe_), "relative/folder\n");
    try {
        make_tar();
        FAIL() << "a relative path was accepted";
    } catch (const bb::common::NoDataFolder& e) {
        EXPECT_EQ(e.reason(), bb::common::NoDataFolder::Reason::pointer_unusable);
        EXPECT_NE(std::string(e.what()).find(bb::common::program_data_dir_path(exe_).string()),
                  std::string::npos) << e.what();
    }
}

// The AppImage keeps its rules: the config folder's pointer, never a file next to the program.
TEST_F(TarGzDirsTest, AnAppImageReadsOnlyTheConfigFoldersPointer) {
    fs::create_directories(tmp_ / "one");
    fs::create_directories(tmp_ / "two");
    write_to(config_pointer_, (tmp_ / "one").string() + "\n");
    write_to(bb::common::program_data_dir_path(exe_), (tmp_ / "two").string() + "\n");
    EXPECT_EQ(make_tar(true).root(), tmp_ / "one");
    fs::remove(config_pointer_);
    EXPECT_THROW(make_tar(true), bb::common::NoDataFolder);
}

TEST_F(TarGzDirsTest, ThePointerFileCanBeWrittenAnywhere) {
    const fs::path file = bb::common::program_data_dir_path(exe_);
    EXPECT_EQ(file, exe_ / "data-dir");
    bb::common::write_data_dir_pointer(tmp_ / "somewhere", file);
    EXPECT_EQ(bb::common::read_data_dir_pointer(file), tmp_ / "somewhere");
    EXPECT_FALSE(fs::exists(config_pointer_));
}

}  // namespace
