// The profiles of the Saves page (docs/design/launcher.md, "Saves"): the folders below
// <data>/profiles that `bb-run --profile` uses, their last play time, the naming rules, creating
// and renaming. No SDL or ImGui in here, so the unit tests cover it on a temporary directory.
#pragma once

#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bb::launcher {

struct ProfileInfo {
    std::string name;
    std::optional<std::time_t> last_played;  // newest change of anything in its folder
};

// Why a name is refused. `ok` means it can be used.
enum class NameProblem { ok, empty, too_long, bad_character, exists };

// Checks `name` against bb-run's rule (1 to 32 characters from A-Z a-z 0-9 _ -). `exists` is only
// reported when `data_dir` is given and has a profile of that name.
NameProblem check_profile_name(std::string_view name,
                               const std::filesystem::path* data_dir = nullptr);

// <data_dir>/profiles/<name>: where bb-run keeps the profile's save data.
std::filesystem::path profile_folder(const std::filesystem::path& data_dir, std::string_view name);

// The profiles in <data_dir>/profiles with a valid name, newest play first, then by name.
// `current` is listed even when it has no folder yet (bb-run makes it on the first run).
std::vector<ProfileInfo> list_profiles(const std::filesystem::path& data_dir,
                                       std::string_view current);

// Makes the folder of a new profile. Returns the problem with the name, or ok. Throws
// std::filesystem::filesystem_error when the folder can't be made.
NameProblem create_profile(const std::filesystem::path& data_dir, std::string_view name);

// Renames the folder of `from` to `to` (also the profile's cache folder when there is one under
// `cache_dir`, which may be empty). Returns the problem with `to`, or ok. A `from` that has no
// folder is fine (nothing to move). Throws std::filesystem::filesystem_error on a failed rename.
NameProblem rename_profile(const std::filesystem::path& data_dir,
                           const std::filesystem::path& cache_dir, std::string_view from,
                           std::string_view to);

// "file://" and the percent-encoded absolute path, for SDL_OpenURL.
std::string file_url(const std::filesystem::path& folder);

}  // namespace bb::launcher
