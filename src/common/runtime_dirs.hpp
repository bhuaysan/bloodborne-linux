// The runtime's own directories: data, cache, config and state under the XDG base directories,
// and the selected profile (docs/design/m3-core-os.md, "Runtime directories").
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace bb::common {

// The default profile, used when --profile is not given.
inline constexpr std::string_view default_profile = "default";

// True for a valid profile name: 1 to 32 characters from [A-Za-z0-9_-].
bool valid_profile_name(std::string_view name);

// The source checkout this binary was built from. RuntimeDirs refuses directories inside it.
std::filesystem::path source_dir();

class RuntimeDirs {
public:
    // Reads XDG_DATA_HOME, XDG_CACHE_HOME, XDG_CONFIG_HOME and XDG_STATE_HOME. A variable that is
    // unset, empty or relative (the XDG spec says to ignore relative values) falls back to its
    // default below $HOME: .local/share, .cache, .config and .local/state. Each gets
    // bloodborne-linux/ appended. Nothing is created here.
    //
    // Throws std::invalid_argument for an invalid profile name, and std::runtime_error when a
    // default is needed but HOME is unset, empty or relative, or when one of the four directories
    // and one of `forbidden` (the game directory) or source_dir() lie inside one another.
    static RuntimeDirs from_environment(std::string_view profile,
                                        const std::vector<std::filesystem::path>& forbidden);

    const std::string& profile() const { return profile_; }

    // $XDG_DATA_HOME/bloodborne-linux and so on.
    const std::filesystem::path& data() const { return data_; }
    const std::filesystem::path& cache() const { return cache_; }
    const std::filesystem::path& config() const { return config_; }
    const std::filesystem::path& state() const { return state_; }

    // data()/profiles/<profile>: save data lives below it.
    std::filesystem::path profile_data() const;
    // cache()/profiles/<profile>: the temporary mount lives below it.
    std::filesystem::path profile_cache() const;
    // state()/logs.
    std::filesystem::path logs() const;

    // Creates `dir` and its missing parents with mode 0700 (existing directories keep their
    // mode) and returns it normalised. `dir` must be absolute and lie inside one of the four
    // directories, else std::logic_error. Throws std::filesystem::filesystem_error when creation
    // fails, a component exists and is not a directory, or a component at or below the base
    // directory is a symlink (it could lead outside the four directories).
    std::filesystem::path ensure(const std::filesystem::path& dir) const;

private:
    std::string profile_;
    std::filesystem::path data_;
    std::filesystem::path cache_;
    std::filesystem::path config_;
    std::filesystem::path state_;
};

}  // namespace bb::common
