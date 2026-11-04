// Settings from the environment and from KEY=value files such as local.env.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace bb::common {

// Value of `key` in a KEY=value file. Lines may start with "export " and values may be quoted.
// The last assignment wins, as in a shell. Returns nothing when the file can't be read, the key is
// missing or its value is empty.
std::optional<std::string> read_env_file(const std::filesystem::path& file, std::string_view key);

// `key` from the environment, else from `env_file`. An empty value counts as unset.
std::optional<std::string> env_or_file(std::string_view key, const std::filesystem::path& env_file);

// The game directory: BB_GAME_DIR from the environment, else from `env_file`.
std::optional<std::filesystem::path> game_dir(const std::filesystem::path& env_file);

}  // namespace bb::common
