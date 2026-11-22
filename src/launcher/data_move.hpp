// Moving the data folder (docs/design/launcher.md, "Moving the data folder"): the config, the
// profiles and the logs are copied to the new folder and checked, then the pointer is switched,
// then the old copy and the cache are removed. Pure logic without SDL, so the tests cover it.
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace bb::launcher {

// Where this package keeps the choice of its data folder, and where "Back to default" goes.
struct DataPlan {
    // The file a change writes: the AppImage's and an unwritable tar.gz folder's pointer in the
    // config folder, else `data-dir` next to the program.
    std::filesystem::path pointer_file;
    // user/ next to the program (tar.gz), else the folder the first start proposes. Empty when
    // none can be proposed.
    std::filesystem::path default_folder;
    // The default is the program's own user/: going back removes `pointer_file` instead of
    // writing it.
    bool default_removes_pointer = false;
    // `pointer_file` can be written. False for a tar.gz whose folder is read-only (the runtime
    // reads data-dir next to the program first, so no other file would take effect), and without a
    // config folder.
    bool changeable = true;
};
struct PlanInput {
    bool appimage = false;
    std::filesystem::path exe_dir;       // the program's directory (the tar.gz's folder)
    std::filesystem::path appimage_file;  // the APPIMAGE variable, else empty
    std::string home;
};
DataPlan plan_data_folder(const PlanInput& in);

// Removes what the runtime's libraries wrote to `folder`/cache: all of cache/ when the folder
// holds only the runtime's own entries, else only cache/system, pipeline and profiles.
void remove_old_cache(const std::filesystem::path& folder);

enum class MoveProblem { none, not_absolute, same_folder, nested, overlaps, is_game, inside_game,
                         not_empty, cannot_create, copy_failed, verify_failed, config_failed,
                         pointer_failed, no_place };

struct MoveRequest {
    std::filesystem::path from;  // the data folder in use
    std::filesystem::path to;
    std::filesystem::path pointer_file;  // DataPlan::pointer_file
    bool remove_pointer = false;         // DataPlan::default_removes_pointer
    std::vector<std::filesystem::path> forbidden;  // the game folder, as RuntimeDirs takes it
    // Fault injection for the tests: called with each file after its copy; true makes that copy
    // fail.
    std::function<bool(const std::filesystem::path&)> fail_after_copy;
};

struct MoveResult {
    MoveProblem problem = MoveProblem::none;
    std::string detail;  // the system's or the check's own message, for the log
    // Old files that could not be removed after the switch. The new folder is in use anyway.
    std::vector<std::filesystem::path> leftovers;
    // Entries of the old folder that are not the runtime's (a shared folder): left in place.
    std::vector<std::filesystem::path> kept;
    bool ok() const { return problem == MoveProblem::none; }
};

// Moves config.toml, profiles/ and logs/ of `from` to `to` (an absolute path that is new, empty or
// holds only game/ and cache/; outside the game and source folders and any dump; not inside or
// around `from`, also by another path to the same folder), verifying every file by size and
// SHA-256. A game_dir stored relative is written to the copy as the absolute path it names (the
// dump stays where it is). The copy is flushed to disk, then the pointer is written (or removed)
// and flushed, and only then config.toml, profiles/, logs/ and the cache (remove_old_cache) of
// `from` go, and `from` itself when that leaves it empty. A failure before the pointer removes
// what was copied and changes nothing else.
MoveResult move_data_folder(const MoveRequest& request);

}  // namespace bb::launcher
