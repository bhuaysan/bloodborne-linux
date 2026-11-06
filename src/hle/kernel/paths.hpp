// Guest paths to host paths through a mount table (docs/design/m3-core-os.md, "Files and paths").
// The file functions resolve every guest path here before they touch the host.
#pragma once

#include <cerrno>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <string_view>

namespace bb::common {
class RuntimeDirs;
}

namespace bb::hle::kernel {

// What the caller will do with the path. Anything that may create, change or remove a file or
// directory (open for writing, O_CREAT, O_TRUNC, mkdir, unlink, rename, truncate) is `write`.
enum class Access { read, write };

// The result of a resolution: a host path, or a host errno.
struct Resolved {
    std::filesystem::path host;  // empty on failure
    int error = 0;               // host errno, 0 on success
    std::string mount;           // the mount's name without "/" ("app0"), empty on failure
    bool mount_root = false;     // the path names the mount point itself
    explicit operator bool() const { return error == 0; }
};

// The guest mount point of the game directory.
inline constexpr std::string_view game_mount = "/app0";

// True for the macOS junk files in a dump: names starting with "._", and ".DS_Store". They are
// never resolved and directory listings hide them.
bool is_junk_name(std::string_view name);

// The host directory of save data mount <n>, profile_data()/savedata/<n>, created if missing.
std::filesystem::path savedata_dir(const common::RuntimeDirs& dirs, unsigned n);
// The host directory of the temporary mount, profile_cache()/temp, created if missing.
std::filesystem::path temp_dir(const common::RuntimeDirs& dirs);

class PathMap {
public:
    // Mounts `game_dir` read-only at /app0, replacing an earlier game mount.
    void mount_game(const std::filesystem::path& game_dir);

    // Mounts `host_dir` read-write at `guest_point`, a single component such as "/savedata0".
    // Returns false if something is mounted there already. Throws std::invalid_argument for a
    // malformed mount point, /app0 or /hostapp.
    bool mount(std::string_view guest_point, const std::filesystem::path& host_dir);

    // Removes the read-write mount at `guest_point`. Returns false if there is none.
    bool unmount(std::string_view guest_point);

    // Maps an absolute guest path to its host path. "." and repeated "/" are dropped and ".." is
    // applied. A trailing "/" or "/." stays a trailing "/", so a file there fails with ENOTDIR on
    // the host. Errors (host errno):
    // - ENOENT: no mount (logged once per first component; /hostapp is refused the same way),
    //   a relative or empty path, ".." above the mount point, or a junk file name;
    // - EROFS: `write` under /app0;
    // - ENAMETOOLONG: 1024 bytes or more, or a component of more than 255 bytes.
    // A path that doesn't exist on the host still resolves (the host call then fails, or creates
    // it).
    Resolved resolve(std::string_view guest, Access access);

private:
    struct Mount {
        std::filesystem::path host;
        bool writable = false;
    };
    void log_refused(const std::string& first, std::string_view why);

    std::shared_mutex mounts_lock_;
    std::map<std::string, Mount, std::less<>> mounts_;  // first component, without "/"
    std::mutex log_lock_;
    std::set<std::string> logged_;
};

// The process-wide mount table that the HLE file functions use.
PathMap& path_map();

// path_map().resolve(guest, access).
inline Resolved resolve_path(std::string_view guest, Access access) {
    return path_map().resolve(guest, access);
}

// resolve_path for a guest C string: the result in `out` and its host errno, EFAULT for null.
inline int resolve_guest_path(const char* path, Access access, Resolved& out) {
    if (path == nullptr) {
        return EFAULT;
    }
    out = resolve_path(path, access);
    return out.error;
}

}  // namespace bb::hle::kernel
