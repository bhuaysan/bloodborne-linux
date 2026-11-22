#include "common/runtime_dirs.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace bb::common {
namespace {

namespace fs = std::filesystem;

// An absolute path from the environment variable `key`, nothing when it is unset or empty.
// A relative value throws: ignoring it would silently write somewhere else.
std::optional<fs::path> absolute_env(const char* key) {
    const char* value = std::getenv(key);
    if (!value || !*value) {
        return std::nullopt;
    }
    fs::path path(value);
    if (!path.is_absolute()) {
        throw std::runtime_error(std::string(key) + " must be an absolute path: " + value);
    }
    return path;
}

// `path` lexically normalised and without a trailing separator.
fs::path normal(const fs::path& path) {
    fs::path result = path.lexically_normal();
    if (!result.has_filename() && result.has_relative_path()) {
        result = result.parent_path();
    }
    return result;
}

// True when `inner` is `outer` or lies below it, compared component by component.
bool is_within(const fs::path& inner, const fs::path& outer) {
    fs::path in = normal(inner);
    fs::path out = normal(outer);
    return std::mismatch(out.begin(), out.end(), in.begin(), in.end()).first == out.end();
}

// `path` normalised, with symlinks resolved where it exists, so the containment checks compare
// real paths.
fs::path real(const fs::path& path) { return normal(fs::weakly_canonical(path)); }

void check_not_overlapping(const fs::path& dir, const fs::path& forbidden,
                           std::string_view hint) {
    fs::path other = normal(fs::weakly_canonical(fs::absolute(forbidden)));
    if (is_within(dir, other) || is_within(other, dir)) {
        std::string message = "runtime directory " + dir.string() + " overlaps " + other.string() +
                              " (game or source directory)";
        message += hint;
        throw std::runtime_error(message);
    }
}

void set_env(const char* key, const fs::path& value) { ::setenv(key, value.c_str(), 1); }

constexpr std::string_view pointer_hint =
    "; remove the data-dir file in the config folder to choose another data folder";

}  // namespace

bool is_appimage_environment(std::string_view appimage, std::string_view appdir,
                             const fs::path& exe_dir) {
    if (appimage.empty() || appdir.empty() || !fs::path(appimage).is_absolute() ||
        !fs::path(appdir).is_absolute()) {
        return false;
    }
    return is_within(real(exe_dir), real(fs::path(appdir)));
}

bool running_from_appimage() {
    const char* appimage = std::getenv("APPIMAGE");
    const char* appdir = std::getenv("APPDIR");
    if (!appimage || !appdir) {
        return false;
    }
    try {
        return is_appimage_environment(appimage, appdir, executable_dir());
    } catch (const std::exception&) {
        return false;  // no /proc/self/exe, or a path that can't be resolved
    }
}

fs::path data_dir_pointer_path() {
    fs::path config_home;
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    if (xdg && *xdg && fs::path(xdg).is_absolute()) {
        config_home = xdg;
    } else if (home && *home && fs::path(home).is_absolute()) {
        config_home = fs::path(home) / ".config";
    } else {
        throw std::runtime_error("cannot find the config folder: set HOME or XDG_CONFIG_HOME");
    }
    return config_home / "bloodborne-linux" / "data-dir";
}

std::optional<fs::path> read_data_dir_pointer() {
    return read_data_dir_pointer(data_dir_pointer_path());
}

fs::path program_data_dir_path(const fs::path& exe_dir) { return exe_dir / "data-dir"; }

std::optional<fs::path> read_data_dir_pointer(const fs::path& file) {
    std::error_code ec;
    const bool exists = fs::exists(file, ec);
    if (ec) {
        throw std::runtime_error(file.string() + ": cannot check the data folder file: " +
                                 ec.message());
    }
    if (!exists) {
        return std::nullopt;
    }
    std::ifstream in(file, std::ios::binary);
    std::string line((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!in && !in.eof()) {
        throw std::runtime_error(file.string() + ": cannot read the data folder file");
    }
    if (line.ends_with('\n')) {
        line.pop_back();  // only the one the writer added: a folder name may end in a space
    }
    if (line.empty()) {
        throw std::runtime_error(file.string() + ": the file is empty; it must hold the absolute "
                                                 "path of the data folder");
    }
    if (line.find('\n') != std::string::npos) {
        throw std::runtime_error(file.string() + ": the file must hold one line, the absolute "
                                                 "path of the data folder");
    }
    fs::path folder(line);
    if (!folder.is_absolute()) {
        throw std::runtime_error(file.string() + ": the data folder must be an absolute path, not " +
                                 line);
    }
    return folder;
}

namespace {

// Writes `text` to `file` (mode 0600, replaced) and flushes it to disk. False on any failure.
bool write_synced(const fs::path& file, const std::string& text) {
    const int fd = ::open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        return false;
    }
    bool ok = true;
    for (std::size_t done = 0; ok && done < text.size();) {
        const ssize_t n = ::write(fd, text.data() + done, text.size() - done);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        ok = n > 0;
        done += ok ? static_cast<std::size_t>(n) : 0;
    }
    ok = ok && ::fsync(fd) == 0;
    return ::close(fd) == 0 && ok;
}

// Flushes the directory entry of a rename. Best effort: some file systems can't sync a folder.
void sync_directory(const fs::path& dir) {
    const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
}

}  // namespace

void write_data_dir_pointer(const fs::path& folder) {
    write_data_dir_pointer(folder, data_dir_pointer_path());
}

void write_data_dir_pointer(const fs::path& folder, const fs::path& file) {
    if (!folder.is_absolute()) {
        throw std::invalid_argument("the data folder must be an absolute path: " + folder.string());
    }
    if (folder.string().find('\n') != std::string::npos) {
        throw std::invalid_argument("the data folder's name must not contain a newline");
    }
    const fs::path dir = file.parent_path();
    std::error_code ec;
    fs::create_directories(dir.parent_path(), ec);
    if (ec || (::mkdir(dir.c_str(), 0700) != 0 && !(errno == EEXIST && fs::is_directory(dir, ec)))) {
        throw std::runtime_error(dir.string() + ": cannot create the folder");
    }
    const fs::path temp = file.string() + ".tmp";
    if (!write_synced(temp, folder.string() + '\n')) {
        fs::remove(temp, ec);
        throw std::runtime_error(file.string() + ": cannot write the data folder file");
    }
    fs::rename(temp, file, ec);
    if (ec) {
        fs::remove(temp, ec);
        throw std::runtime_error(file.string() + ": cannot write the data folder file");
    }
    sync_directory(dir);
}

bool valid_profile_name(std::string_view name) {
    if (name.empty() || name.size() > 32) {
        return false;
    }
    for (char c : name) {
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

fs::path source_dir() { return fs::path(BB_SOURCE_DIR); }

fs::path executable_dir() {
    std::error_code ec;
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (ec || !exe.is_absolute()) {
        throw std::runtime_error("cannot find the running program's folder (/proc/self/exe)");
    }
    return exe.parent_path();
}

namespace {

// The folder a pointer file names, after the checks of an AppImage's data folder. A folder that is
// gone (an unmounted drive) is not recreated: the player decides.
fs::path pointed_root(const fs::path& pointer) {
    std::error_code ec;
    if (!fs::is_directory(pointer, ec)) {
        throw NoDataFolder(NoDataFolder::Reason::folder_missing,
                           "the data folder " + pointer.string() +
                               " does not exist (an unmounted drive?): start bb-launcher once "
                               "to choose it again, or set BB_USER_DIR",
                           pointer);
    }
    if (::access(pointer.c_str(), W_OK | X_OK) != 0) {
        throw NoDataFolder(NoDataFolder::Reason::folder_unwritable,
                           "the data folder " + pointer.string() +
                               " can't be written: start bb-launcher once to choose another, "
                               "or set BB_USER_DIR",
                           pointer);
    }
    return pointer;
}

// read_data_dir_pointer(file) with an unusable file reported as NoDataFolder.
std::optional<fs::path> read_pointer_or_throw(const fs::path& file) {
    try {
        return read_data_dir_pointer(file);
    } catch (const std::runtime_error& e) {
        throw NoDataFolder(NoDataFolder::Reason::pointer_unusable,
                           std::string(e.what()) +
                               "; start bb-launcher once to choose the data folder again, or "
                               "set BB_USER_DIR");
    }
}

}  // namespace

bool default_data_folder_usable(const fs::path& exe_dir) {
    return ::access(exe_dir.c_str(), W_OK | X_OK) == 0 ||
           ::access((exe_dir / "user").c_str(), W_OK | X_OK) == 0;
}

RuntimeDirs RuntimeDirs::from_environment(std::string_view profile,
                                          const std::vector<fs::path>& forbidden) {
    Package package;
    package.appimage = running_from_appimage();
    const char* user = std::getenv("BB_USER_DIR");
    if (!user || !*user) {
        package.exe_dir = executable_dir();  // only needed without BB_USER_DIR
    }
    return from_environment(profile, forbidden, package);
}

RuntimeDirs RuntimeDirs::from_environment(std::string_view profile,
                                          const std::vector<fs::path>& forbidden,
                                          const Package& package) {
    if (!valid_profile_name(profile)) {
        throw std::invalid_argument("invalid profile name '" + std::string(profile) +
                                    "': use 1 to 32 characters from A-Z, a-z, 0-9, _ and -");
    }
    RuntimeDirs dirs;
    dirs.profile_ = std::string(profile);
    const auto user_dir = absolute_env("BB_USER_DIR");
    const auto cache_dir = absolute_env("BB_CACHE_DIR");
    fs::path root;
    std::string_view hint = "; set BB_USER_DIR to a directory outside both";
    const auto no_choice = [] {
        return NoDataFolder(
            NoDataFolder::Reason::not_chosen,
            "no data folder chosen yet: start bb-launcher once to choose where the game keeps "
            "its data, or set BB_USER_DIR");
    };
    if (user_dir) {
        root = *user_dir;
        hint = "";
    } else if (package.appimage) {
        const auto pointer = read_pointer_or_throw(data_dir_pointer_path());
        if (!pointer) {
            throw no_choice();
        }
        root = pointed_root(*pointer);
        hint = pointer_hint;
    } else if (const auto next_to = read_pointer_or_throw(program_data_dir_path(package.exe_dir))) {
        root = pointed_root(*next_to);
        hint = "; remove the data-dir file next to the program to use user/ again";
    } else if (default_data_folder_usable(package.exe_dir)) {
        root = package.exe_dir / "user";
    } else {
        // A folder that can't be written (copied to /usr/bin, say): the config folder's pointer,
        // which only the launcher's choice writes, else the launcher asks.
        std::optional<fs::path> pointer;
        try {
            pointer = read_pointer_or_throw(data_dir_pointer_path());
        } catch (const NoDataFolder&) {
            throw;
        } catch (const std::runtime_error&) {
            // no HOME and no XDG_CONFIG_HOME: nowhere to keep a choice either
        }
        if (!pointer) {
            throw no_choice();
        }
        root = pointed_root(*pointer);
        hint = pointer_hint;
    }
    dirs.root_ = real(root);
    dirs.data_ = dirs.root_;
    dirs.config_ = dirs.root_;
    dirs.state_ = dirs.root_;
    // Resolved like the root: a user/cache symlink into the game directory is caught by the overlap
    // check below, since shadPS4's cache and the drivers create their folders without ensure().
    dirs.cache_ = real(cache_dir ? *cache_dir : dirs.root_ / "cache");

    std::vector<fs::path> refused = forbidden;
    refused.push_back(source_dir());
    for (const fs::path& other : refused) {
        // In an AppImage a dump may lie in game/ inside the data folder (the game never writes
        // there), so the root may contain it.
        if (!(package.appimage && is_within(real(other), dirs.root_ / "game"))) {
            check_not_overlapping(dirs.root_, other, hint);
        }
        check_not_overlapping(dirs.cache_, other, cache_dir ? "" : hint);
    }
    return dirs;
}

fs::path RuntimeDirs::profile_data() const { return data_ / "profiles" / profile_; }

fs::path RuntimeDirs::profile_cache() const { return cache_ / "profiles" / profile_; }

fs::path RuntimeDirs::logs() const { return state_ / "logs"; }

fs::path RuntimeDirs::ensure(const fs::path& dir) const {
    fs::path target = normal(dir);
    const fs::path* owner = nullptr;
    for (const fs::path* base : {&data_, &cache_, &config_, &state_}) {
        if (!owner && is_within(target, *base)) {
            owner = base;
        }
    }
    if (!target.is_absolute() || !owner) {
        throw std::logic_error("not a runtime directory: " + dir.string());
    }
    // Create each missing component top-down, so a missing root and its missing parents get 0700
    // too.
    fs::path current;
    for (const fs::path& part : target) {
        current /= part;
        std::error_code ec;
        // The containment check above is lexical. A symlink at or below the base directory
        // could point anywhere (the game directory, say), so refuse to walk through one. The
        // base's parents had their symlinks resolved when the base was computed.
        if (is_within(current, *owner) && fs::is_symlink(fs::symlink_status(current, ec))) {
            throw fs::filesystem_error(
                "runtime directory component is a symlink", current,
                std::make_error_code(std::errc::too_many_symbolic_link_levels));
        }
        if (fs::is_directory(current, ec)) {
            continue;
        }
        if (::mkdir(current.c_str(), 0700) == 0) {
            continue;
        }
        int err = errno;
        if (err == EEXIST && fs::is_directory(current, ec)) {
            continue;  // created concurrently
        }
        throw fs::filesystem_error("cannot create runtime directory", current,
                                   std::error_code(err == EEXIST ? ENOTDIR : err,
                                                   std::generic_category()));
    }
    return target;
}

void check_data_folder(const fs::path& folder, const std::vector<fs::path>& forbidden) {
    const fs::path root = real(folder);
    std::vector<fs::path> refused = forbidden;
    refused.push_back(source_dir());
    for (const fs::path& other : refused) {
        if (!is_within(real(other), root / "game")) {
            check_not_overlapping(root, other, "");
        }
    }
}

void redirect_library_caches(const RuntimeDirs& dirs) {
    const fs::path system = dirs.cache() / "system";
    try {
        dirs.ensure(system / "nvidia");  // NVIDIA's driver may not create its folder itself
    } catch (const std::exception&) {
        // The caller reports a folder it can't write; the libraries then fail to write as well.
    }
    set_env("XDG_CACHE_HOME", system);
    set_env("MESA_SHADER_CACHE_DIR", system);
    set_env("__GL_SHADER_DISK_CACHE_PATH", system / "nvidia");
}

}  // namespace bb::common
