// The runtime's own directories: everything it writes lives in one portable folder, user/ next to
// the binary, and the selected profile (docs/design/m3-core-os.md, "Runtime directories").
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

// The directory of the running executable, from /proc/self/exe. Throws std::runtime_error when
// that link can't be read.
std::filesystem::path executable_dir();

class RuntimeDirs {
public:
    // The root is BB_USER_DIR when that is set to an absolute path (development, tests, the rig),
    // else user/ in executable_dir(). config(), data() and state() are the root, cache() is
    // root/cache, or BB_CACHE_DIR when that is set to an absolute path (the rig keeps a warm cache
    // apart from the per-run root). Empty values count as unset. XDG variables and HOME are not
    // read. Nothing is created here.
    //
    // Throws std::invalid_argument for an invalid profile name, and std::runtime_error for a
    // relative BB_USER_DIR or BB_CACHE_DIR, when the executable's directory is needed but can't be
    // read, or when one of the four directories and
    // one of `forbidden` (the game directory) or source_dir() lie inside one another.
    static RuntimeDirs from_environment(std::string_view profile,
                                        const std::vector<std::filesystem::path>& forbidden);

    const std::string& profile() const { return profile_; }

    // The portable folder: config.toml, profiles/, cache/ and logs/ below it.
    const std::filesystem::path& root() const { return root_; }
    // root(), except cache() as described above.
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
    std::filesystem::path root_;
    std::filesystem::path data_;
    std::filesystem::path cache_;
    std::filesystem::path config_;
    std::filesystem::path state_;
};

// Points the caches that libraries write on their own at cache()/system: XDG_CACHE_HOME (Mesa's
// shader cache, fontconfig), MESA_SHADER_CACHE_DIR and __GL_SHADER_DISK_CACHE_PATH (NVIDIA), so
// nothing lands in ~/.cache. XDG_CONFIG_HOME and XDG_DATA_HOME stay: libraries only read them.
// Creates cache()/system when it can; a folder that can't be written is left to the caller's own
// error. Call it before SDL, the Vulkan loader or fontconfig start and before threads that read the
// environment.
void redirect_library_caches(const RuntimeDirs& dirs);

}  // namespace bb::common
