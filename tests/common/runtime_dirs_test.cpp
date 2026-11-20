#include "common/runtime_dirs.hpp"

#include <array>
#include <cstdlib>
#include <fstream>
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

constexpr std::array<const char*, 9> env_keys = {
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
    // Empty and relative values count as unset.
    for (const std::string value : {"", "relative/user"}) {
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

}  // namespace
