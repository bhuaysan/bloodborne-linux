#include "common/env.hpp"

#include <cstdlib>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

namespace {

namespace fs = std::filesystem;

class EnvFileTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() /
               ("bb-env-test-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "-" +
                ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::create_directories(dir_);
        file_ = dir_ / "local.env";
    }
    void TearDown() override { fs::remove_all(dir_); }

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

}  // namespace
