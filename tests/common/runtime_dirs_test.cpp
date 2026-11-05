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

constexpr std::array<const char*, 5> env_keys = {"HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME",
                                                 "XDG_CONFIG_HOME", "XDG_STATE_HOME"};

// Every test runs with HOME pointing into a fresh temporary directory and all XDG variables
// unset, and restores the environment afterwards. Nothing is written outside that directory.
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
        set("HOME", (tmp_ / "home").string());
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

TEST_F(RuntimeDirsTest, DefaultsBelowHome) {
    RuntimeDirs dirs = make();
    fs::path home = tmp_ / "home";
    EXPECT_EQ(dirs.profile(), "default");
    EXPECT_EQ(dirs.data(), home / ".local/share/bloodborne-linux");
    EXPECT_EQ(dirs.cache(), home / ".cache/bloodborne-linux");
    EXPECT_EQ(dirs.config(), home / ".config/bloodborne-linux");
    EXPECT_EQ(dirs.state(), home / ".local/state/bloodborne-linux");
    EXPECT_EQ(dirs.profile_data(), dirs.data() / "profiles/default");
    EXPECT_EQ(dirs.profile_cache(), dirs.cache() / "profiles/default");
    EXPECT_EQ(dirs.logs(), dirs.state() / "logs");
    EXPECT_FALSE(fs::exists(home)) << "nothing is created before first use";
}

TEST_F(RuntimeDirsTest, UsesSetVariables) {
    set("XDG_DATA_HOME", (tmp_ / "d").string());
    set("XDG_CACHE_HOME", (tmp_ / "c").string());
    set("XDG_CONFIG_HOME", (tmp_ / "k").string());
    set("XDG_STATE_HOME", (tmp_ / "s/").string());
    RuntimeDirs dirs = make("test-1_A");
    EXPECT_EQ(dirs.data(), tmp_ / "d/bloodborne-linux");
    EXPECT_EQ(dirs.cache(), tmp_ / "c/bloodborne-linux");
    EXPECT_EQ(dirs.config(), tmp_ / "k/bloodborne-linux");
    EXPECT_EQ(dirs.state(), tmp_ / "s/bloodborne-linux");
    EXPECT_EQ(dirs.profile_data(), tmp_ / "d/bloodborne-linux/profiles/test-1_A");
}

TEST_F(RuntimeDirsTest, EmptyAndRelativeVariablesUseDefaults) {
    set("XDG_DATA_HOME", "");
    set("XDG_CACHE_HOME", "relative/cache");
    RuntimeDirs dirs = make();
    EXPECT_EQ(dirs.data(), tmp_ / "home/.local/share/bloodborne-linux");
    EXPECT_EQ(dirs.cache(), tmp_ / "home/.cache/bloodborne-linux");
}

TEST_F(RuntimeDirsTest, NeedsHomeOnlyForDefaults) {
    ::unsetenv("HOME");
    EXPECT_THROW(make(), std::runtime_error);
    set("HOME", "");
    EXPECT_THROW(make(), std::runtime_error);
    for (const char* key : env_keys) {
        set(key, (tmp_ / key).string());
    }
    ::unsetenv("HOME");
    EXPECT_EQ(make().data(), tmp_ / "XDG_DATA_HOME/bloodborne-linux");
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
    set("XDG_DATA_HOME", (game / "data").string());
    EXPECT_THROW(make("default", {game}), std::runtime_error);
    // Also through a symlink and with a trailing slash on the game directory.
    fs::create_directory_symlink(game, tmp_ / "link");
    set("XDG_DATA_HOME", (tmp_ / "link/x").string());
    EXPECT_THROW(make("default", {tmp_ / "game/"}), std::runtime_error);
    // And the other way round: the game inside a runtime directory.
    set("XDG_DATA_HOME", (tmp_ / "outer").string());
    EXPECT_THROW(make("default", {tmp_ / "outer/bloodborne-linux/game"}), std::runtime_error);
    // A sibling with a common prefix is fine.
    set("XDG_DATA_HOME", (tmp_ / "game-data").string());
    EXPECT_NO_THROW(make("default", {game}));
    EXPECT_FALSE(fs::exists(tmp_ / "game-data"));
    EXPECT_TRUE(fs::is_empty(game));
}

TEST_F(RuntimeDirsTest, RefusesDirectoriesInsideTheSourceCheckout) {
    // Only computes paths: nothing is created in the checkout.
    set("XDG_CACHE_HOME", (bb::common::source_dir() / "build").string());
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
    set("XDG_STATE_HOME", tmp_.string());
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
    fs::create_directories(dirs.state().parent_path());
    fs::create_directory_symlink(game, dirs.state());
    EXPECT_THROW(dirs.ensure(dirs.logs()), fs::filesystem_error);
    EXPECT_TRUE(fs::is_empty(game));
}

}  // namespace
