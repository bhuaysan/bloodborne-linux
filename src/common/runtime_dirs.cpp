#include "common/runtime_dirs.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <system_error>

#include <sys/stat.h>

namespace bb::common {
namespace {

namespace fs = std::filesystem;

// An absolute path from the environment variable `key`. Unset, empty and relative values count
// as unset.
std::optional<fs::path> absolute_env(const char* key) {
    const char* value = std::getenv(key);
    if (!value || !*value) {
        return std::nullopt;
    }
    fs::path path(value);
    if (!path.is_absolute()) {
        return std::nullopt;
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

void check_not_overlapping(const fs::path& dir, const fs::path& forbidden, bool from_variable) {
    fs::path other = normal(fs::weakly_canonical(fs::absolute(forbidden)));
    if (is_within(dir, other) || is_within(other, dir)) {
        std::string message = "runtime directory " + dir.string() + " overlaps " + other.string() +
                              " (game or source directory)";
        if (!from_variable) {
            message += "; set BB_USER_DIR to a directory outside both";
        }
        throw std::runtime_error(message);
    }
}

void set_env(const char* key, const fs::path& value) { ::setenv(key, value.c_str(), 1); }

}  // namespace

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

RuntimeDirs RuntimeDirs::from_environment(std::string_view profile,
                                          const std::vector<fs::path>& forbidden) {
    if (!valid_profile_name(profile)) {
        throw std::invalid_argument("invalid profile name '" + std::string(profile) +
                                    "': use 1 to 32 characters from A-Z, a-z, 0-9, _ and -");
    }
    RuntimeDirs dirs;
    dirs.profile_ = std::string(profile);
    const auto user_dir = absolute_env("BB_USER_DIR");
    const auto cache_dir = absolute_env("BB_CACHE_DIR");
    dirs.root_ = real(user_dir ? *user_dir : executable_dir() / "user");
    dirs.data_ = dirs.root_;
    dirs.config_ = dirs.root_;
    dirs.state_ = dirs.root_;
    dirs.cache_ = cache_dir ? real(*cache_dir) : dirs.root_ / "cache";

    std::vector<fs::path> refused = forbidden;
    refused.push_back(source_dir());
    for (const fs::path& other : refused) {
        check_not_overlapping(dirs.root_, other, user_dir.has_value());
        check_not_overlapping(dirs.cache_, other, user_dir.has_value() || cache_dir.has_value());
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
