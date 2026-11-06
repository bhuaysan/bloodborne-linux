// Test helpers for tests that need the user's game dump.
#pragma once

#include <filesystem>
#include <optional>

#include <gtest/gtest.h>

namespace bb::test {

// local.env in the main checkout (also from a linked worktree).
std::filesystem::path local_env_file();

// BB_GAME_DIR from the environment, else from local.env. Nothing when neither sets it.
std::optional<std::filesystem::path> game_dir();

// analysis/ in the main checkout: the untracked outputs of the tools (also from a linked
// worktree). It may not exist.
std::filesystem::path analysis_dir();

}  // namespace bb::test

// Skips the current test when the game isn't available. Declares `game` as its directory.
#define BB_REQUIRE_GAME(game)                                         \
    auto game##_opt = ::bb::test::game_dir();                         \
    if (!game##_opt) {                                                \
        GTEST_SKIP() << "BB_GAME_DIR is not set (environment or local.env)"; \
    }                                                                 \
    const std::filesystem::path game = *game##_opt
