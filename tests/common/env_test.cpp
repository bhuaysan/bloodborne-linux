#include "common/env.hpp"

#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>

#include <unistd.h>

#include <gtest/gtest.h>

namespace {

namespace fs = std::filesystem;

class EnvFileTest : public ::testing::Test {
protected:
    void SetUp() override {
        // mkdtemp gives each run its own directory, also when presets or worktrees run in parallel.
        std::string templ = (fs::temp_directory_path() / "bb-env-test-XXXXXX").string();
        ASSERT_NE(::mkdtemp(templ.data()), nullptr);
        dir_ = templ;
        file_ = dir_ / "local.env";
    }
    void TearDown() override {
        if (!dir_.empty()) {
            fs::remove_all(dir_);
        }
    }

    void write(const std::string& text) { std::ofstream(file_) << text; }

    fs::path dir_;
    fs::path file_;
};

TEST_F(EnvFileTest, ReadsPlainAssignment) {
    write("BB_TEST_KEY=/some/dir\n");
    EXPECT_EQ(bb::common::read_env_file(file_, "BB_TEST_KEY"), "/some/dir");
}

TEST_F(EnvFileTest, AcceptsExportQuotesAndSpaces) {
    write("  export BB_TEST_KEY = \"/quoted dir\"  \n");
    EXPECT_EQ(bb::common::read_env_file(file_, "BB_TEST_KEY"), "/quoted dir");
}

TEST_F(EnvFileTest, LastAssignmentWins) {
    write("BB_TEST_KEY=first\nOTHER=x\nBB_TEST_KEY='second'\n");
    EXPECT_EQ(bb::common::read_env_file(file_, "BB_TEST_KEY"), "second");
}

TEST_F(EnvFileTest, IgnoresOtherKeysAndLinesWithoutEquals) {
    write("# comment\nBB_TEST_KEY_LONGER=no\nBB_TEST_KEY\n");
    EXPECT_EQ(bb::common::read_env_file(file_, "BB_TEST_KEY"), std::nullopt);
}

TEST_F(EnvFileTest, EmptyValueCountsAsUnset) {
    write("BB_TEST_KEY=\"\"\n");
    EXPECT_EQ(bb::common::read_env_file(file_, "BB_TEST_KEY"), std::nullopt);
}

TEST_F(EnvFileTest, MissingFileGivesNothing) {
    EXPECT_EQ(bb::common::read_env_file(dir_ / "absent.env", "BB_TEST_KEY"), std::nullopt);
}

TEST_F(EnvFileTest, EnvironmentWinsOverFile) {
    write("BB_TEST_KEY=from-file\n");
    ::setenv("BB_TEST_KEY", "from-env", 1);
    EXPECT_EQ(bb::common::env_or_file("BB_TEST_KEY", file_), "from-env");
    ::setenv("BB_TEST_KEY", "", 1);
    EXPECT_EQ(bb::common::env_or_file("BB_TEST_KEY", file_), "from-file");
    ::unsetenv("BB_TEST_KEY");
    EXPECT_EQ(bb::common::env_or_file("BB_TEST_KEY", file_), "from-file");
}

// Saves an environment variable and restores it, or unsets it again, when the test ends.
class SavedEnv {
public:
    explicit SavedEnv(const char* name) : name_(name) {
        if (const char* value = std::getenv(name)) {
            value_ = value;
        }
    }
    ~SavedEnv() {
        if (value_) {
            ::setenv(name_, value_->c_str(), 1);
        } else {
            ::unsetenv(name_);
        }
    }
    SavedEnv(const SavedEnv&) = delete;
    SavedEnv& operator=(const SavedEnv&) = delete;

private:
    const char* name_;
    std::optional<std::string> value_;
};

class GameDirTest : public EnvFileTest {
protected:
    void SetUp() override {
        EnvFileTest::SetUp();
        ::unsetenv("BB_GAME_DIR");
    }
    SavedEnv home_{"HOME"};
    SavedEnv game_{"BB_GAME_DIR"};
};

TEST_F(GameDirTest, EnvironmentWinsOverFile) {
    fs::create_directories(dir_ / "a");
    fs::create_directories(dir_ / "b");
    write("BB_GAME_DIR=" + (dir_ / "a").string() + "\n");
    EXPECT_EQ(bb::common::game_dir(file_), fs::canonical(dir_ / "a"));
    ::setenv("BB_GAME_DIR", (dir_ / "b").c_str(), 1);
    EXPECT_EQ(bb::common::game_dir(file_), fs::canonical(dir_ / "b"));
}

TEST_F(GameDirTest, ExpandsHomeAndResolves) {
    fs::create_directories(dir_ / "games" / "dump");
    ::setenv("HOME", dir_.c_str(), 1);
    write("BB_GAME_DIR=~/games/./dump\n");
    EXPECT_EQ(bb::common::game_dir(file_), fs::canonical(dir_ / "games" / "dump"));
}

TEST_F(GameDirTest, MakesRelativePathsAbsolute) {
    write("BB_GAME_DIR=relative/dump\n");
    auto dir = bb::common::game_dir(file_);
    ASSERT_TRUE(dir);
    EXPECT_TRUE(dir->is_absolute());
    EXPECT_EQ(dir->filename(), "dump");
}

TEST_F(GameDirTest, UnsetGivesNothing) {
    EXPECT_EQ(bb::common::game_dir(file_), std::nullopt);
}

}  // namespace
