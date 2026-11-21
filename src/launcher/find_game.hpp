// The first-start search for the game folder (docs/design/launcher.md, "First start"): walks a few
// directory trees for folders that hold eboot.bin and sce_sys/param.sfo and pass the setup check
// of src/common/game_folder.hpp. No SDL in here; the launcher runs it on a thread of its own.
#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "common/game_folder.hpp"

namespace bb::launcher {

struct FindOptions {
    int max_depth = 4;  // a root's own children are level 1
    std::chrono::milliseconds time_limit{2000};
    std::string_view eboot_sha256 = common::default_eboot_sha256;
};

struct FindResult {
    std::vector<std::filesystem::path> folders;  // passing the check, sorted, shallow ones first
    bool timed_out = false;
    bool cancelled = false;
    std::chrono::milliseconds elapsed{0};
};

// $HOME, /run/media/$USER and /media. Empty values are left out.
std::vector<std::filesystem::path> default_search_roots(std::string_view home, std::string_view user);

// The game/ folder next to the launcher (docs/design/launcher.md, "One portable folder"): the
// folders that pass the check among `dir` itself and its direct children (not hidden; a symbolic
// link to a folder counts), with symlinks resolved, sorted. Empty when `dir` doesn't exist.
std::vector<std::filesystem::path> find_in_game_dir(
    const std::filesystem::path& dir, std::string_view eboot_sha256 = common::default_eboot_sha256);

// Walks `roots` breadth first, skipping hidden directories (a leading dot) and symbolic links, and
// stops at `options.time_limit` or when `cancel` is set. A folder with eboot.bin and
// sce_sys/param.sfo is a candidate and is not searched further; it is a hit when the full check
// passes. A folder reached through two roots counts once.
FindResult find_game_folders(const std::vector<std::filesystem::path>& roots,
                             const FindOptions& options, const std::atomic<bool>& cancel);

}  // namespace bb::launcher
