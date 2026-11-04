#include "support/game.hpp"

#include <gtest/gtest.h>

namespace {

namespace fs = std::filesystem;

// local.env is looked up in the main checkout, also from a linked worktree, where .git is a file.
TEST(TestSupport, LocalEnvLivesInTheMainCheckout) {
    EXPECT_TRUE(fs::is_directory(bb::test::local_env_file().parent_path() / ".git"));
}

TEST(TestSupport, RequireGameFindsTheDump) {
    BB_REQUIRE_GAME(game);
    EXPECT_TRUE(fs::is_regular_file(game / "eboot.bin"));
}

}  // namespace
