#include "support/game.hpp"

#include "common/env.hpp"

namespace bb::test {

std::filesystem::path local_env_file() {
    return std::filesystem::path(BB_MAIN_CHECKOUT) / "local.env";
}

std::filesystem::path analysis_dir() {
    return std::filesystem::path(BB_MAIN_CHECKOUT) / "analysis";
}

std::optional<std::filesystem::path> game_dir() {
    return common::game_dir(local_env_file());
}

}  // namespace bb::test
