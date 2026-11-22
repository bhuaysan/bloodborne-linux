// The runtime's own directories: everything it writes lives in one portable folder, user/ next to
// the binary (the data folder the player chose, inside an AppImage), and the selected profile
// (docs/design/m3-core-os.md, "Runtime directories").
#pragma once

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
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

// True when `appimage` (the APPIMAGE variable) and `appdir` (APPDIR) are absolute paths and the
// directory `exe_dir` lies inside `appdir`, with symlinks resolved. The runtime exports both
// variables to every child, so a program of another package started from an AppImage (Heroic, a
// terminal) sees APPIMAGE as well; only one that runs from the mount counts.
bool is_appimage_environment(std::string_view appimage, std::string_view appdir,
                             const std::filesystem::path& exe_dir);

// True when the program runs from an AppImage: is_appimage_environment for the real APPIMAGE and
// APPDIR variables and executable_dir(). The AppImage's own folder is a read-only mount, so its
// data folder is chosen by the player instead.
bool running_from_appimage();

// Thrown by RuntimeDirs::from_environment when there is no usable data folder: in an AppImage or in
// a tar.gz folder that can't be written, none chosen yet; in any package the pointer file is
// unusable, or the folder it names is gone or can't be written (an unmounted USB stick, say). The
// player has to start bb-launcher once to choose one (or set BB_USER_DIR); the launcher shows
// what() on its first-start screen's account of the reason.
class NoDataFolder : public std::runtime_error {
public:
    enum class Reason { not_chosen, pointer_unusable, folder_missing, folder_unwritable };
    NoDataFolder(Reason reason, const std::string& what, std::filesystem::path folder = {})
        : std::runtime_error(what), reason_(reason), folder_(std::move(folder)) {}
    Reason reason() const { return reason_; }
    // The folder the pointer file names, for the last two reasons.
    const std::filesystem::path& folder() const { return folder_; }

private:
    Reason reason_;
    std::filesystem::path folder_;
};

// The pointer file of the AppImage's data folder: <config home>/bloodborne-linux/data-dir, where
// the config home is XDG_CONFIG_HOME when that is an absolute path, else $HOME/.config. It is the
// only thing the game writes outside its data folder, and only inside an AppImage or a tar.gz folder
// that can't be written (or after the player moved the data folder there). Throws
// std::runtime_error when neither variable gives an absolute config home.
std::filesystem::path data_dir_pointer_path();

// The absolute folder the pointer file names (one final newline removed, nothing else: a folder
// name may end in a space), nothing when the file doesn't exist. Throws std::runtime_error naming
// the file when it can't be checked or read, is empty, holds more than one line or a relative
// path. Reads the file whether or not this is an AppImage.
std::optional<std::filesystem::path> read_data_dir_pointer();
// The same for any file in that format.
std::optional<std::filesystem::path> read_data_dir_pointer(const std::filesystem::path& file);

// The pointer file of the tar.gz package: `data-dir` in the program's own directory, the same
// format. A player who moved the data folder of the tar.gz keeps that choice next to the program,
// so deleting the program's folder removes everything.
std::filesystem::path program_data_dir_path(const std::filesystem::path& exe_dir);

// True when the tar.gz's own directory can hold the default user/: it can be written, or it has a
// writable user/ (an administrator made it for the player).
bool default_data_folder_usable(const std::filesystem::path& exe_dir);

// Writes `folder` (absolute and without a newline, else std::invalid_argument) to the pointer
// file: the folder around it is created with mode 0700, the content goes to a temporary file that
// is flushed to disk, renamed over the old one, and the folder is flushed too. Throws
// std::runtime_error when that fails.
void write_data_dir_pointer(const std::filesystem::path& folder);
// The same for any file in that format (the folder around it must exist or be creatable).
void write_data_dir_pointer(const std::filesystem::path& folder, const std::filesystem::path& file);

// Throws std::runtime_error when the absolute `folder` and one of `forbidden` (the game directory)
// or source_dir() lie inside one another. A game directory inside folder/game is allowed, as in
// RuntimeDirs::from_environment.
void check_data_folder(const std::filesystem::path& folder,
                       const std::vector<std::filesystem::path>& forbidden);

class RuntimeDirs {
public:
    // The package the program runs from: its directory and whether it is an AppImage.
    struct Package {
        std::filesystem::path exe_dir;
        bool appimage = false;
    };

    // The root is BB_USER_DIR when that is set to an absolute path (development, tests, the rig),
    // else, in an AppImage (running_from_appimage), the folder in the config folder's pointer file
    // (throws NoDataFolder when there is none, the file is unusable, or the folder is gone or
    // can't be written: it is never created here). Outside an AppImage (the tar.gz) the order is:
    // the folder in `data-dir` next to the program (same checks), else, only when the program's
    // directory can't hold user/ (it can't be written and has no writable user/), the config
    // folder's pointer file (NoDataFolder::not_chosen when there is none), else user/ next to the
    // program. A tar.gz whose own directory is writable never reads the config folder's file: it
    // belongs to an AppImage (docs/design/launcher.md, "Moving the data folder"). config(), data()
    // and state() are the root, cache() is
    // root/cache, or BB_CACHE_DIR when that is set to an absolute path (the rig keeps a warm cache
    // apart from the per-run root). Empty values count as unset. XDG variables and HOME are not
    // read. Nothing is created here.
    //
    // Throws std::invalid_argument for an invalid profile name, and std::runtime_error for a
    // relative BB_USER_DIR or BB_CACHE_DIR, when the executable's
    // directory is needed but can't be read, or when one of the four directories and
    // one of `forbidden` (the game directory) or source_dir() lie inside one another (in an
    // AppImage a game directory inside root()/game is allowed).
    static RuntimeDirs from_environment(std::string_view profile,
                                        const std::vector<std::filesystem::path>& forbidden);
    // The same for a given package, for the launcher's tests (exe_dir is unused with BB_USER_DIR).
    static RuntimeDirs from_environment(std::string_view profile,
                                        const std::vector<std::filesystem::path>& forbidden,
                                        const Package& package);

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
