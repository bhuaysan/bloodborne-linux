#include "support/game.hpp"

#include <gtest/gtest.h>

namespace {

namespace fs = std::filesystem;

// local.env is looked up in the main checkout, also from a linked worktree, where .git is a file.
TEST(TestSupport, LocalEnvLivesInTheMainCheckout) {
    if (!BB_IN_GIT_CHECKOUT) {
        GTEST_SKIP() << "not a git checkout: local.env is looked up in the source directory";
    }
    EXPECT_TRUE(fs::is_directory(bb::test::local_env_file().parent_path() / ".git"));
}

TEST(TestSupport, AnalysisLivesInTheMainCheckout) {
    if (!BB_IN_GIT_CHECKOUT) {
        GTEST_SKIP() << "not a git checkout: analysis/ is looked up in the source directory";
    }
    EXPECT_EQ(bb::test::analysis_dir(), bb::test::local_env_file().parent_path() / "analysis");
}

TEST(TestSupport, RequireGameFindsTheDump) {
    BB_REQUIRE_GAME(game);
    EXPECT_TRUE(fs::is_regular_file(game / "eboot.bin"));
}

}  // namespace
