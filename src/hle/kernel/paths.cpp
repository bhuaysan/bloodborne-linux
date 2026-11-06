#include "hle/kernel/paths.hpp"

#include <cerrno>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/log.hpp"
#include "common/runtime_dirs.hpp"
#include "hle/kernel/abi.hpp"

namespace bb::hle::kernel {

namespace fs = std::filesystem;

namespace {

// The guest's PATH_MAX (including the NUL) and NAME_MAX (docs/formats/kernel-abi.md).
constexpr std::size_t guest_path_max = 1024;
constexpr std::size_t guest_name_max = 255;

// Guest debug output, /hostapp/%s in the eboot (0x47e012c). It has no host counterpart.
constexpr std::string_view hostapp = "hostapp";

Resolved fail(int host_errno) {
    Resolved r;
    r.error = guest_errno(host_errno).value();
    return r;
}

// "/name" with one component that is neither "." nor "..". Returns the name.
std::string_view mount_name(std::string_view guest_point) {
    const std::string_view name = guest_point.substr(guest_point.empty() ? 0 : 1);
    if (guest_point.empty() || guest_point[0] != '/' || name.empty() || name == "." ||
        name == ".." || name.find('/') != std::string_view::npos ||
        name.size() > guest_name_max) {
        throw std::invalid_argument("malformed guest mount point: " + std::string(guest_point));
    }
    return name;
}

}  // namespace

bool is_junk_name(std::string_view name) {
    return name.starts_with("._") || name == ".DS_Store";
}

fs::path savedata_dir(const common::RuntimeDirs& dirs, unsigned n) {
    return dirs.ensure(dirs.profile_data() / "savedata" / std::to_string(n));
}

fs::path temp_dir(const common::RuntimeDirs& dirs) {
    return dirs.ensure(dirs.profile_cache() / "temp");
}

void PathMap::mount_game(const fs::path& game_dir) {
    std::unique_lock lock(mounts_lock_);
    mounts_[std::string(game_mount.substr(1))] = Mount{game_dir, false};
}

bool PathMap::mount(std::string_view guest_point, const fs::path& host_dir) {
    const std::string_view name = mount_name(guest_point);
    if (guest_point == game_mount || name == hostapp) {
        throw std::invalid_argument("reserved guest mount point: " + std::string(guest_point));
    }
    std::unique_lock lock(mounts_lock_);
    return mounts_.emplace(std::string(name), Mount{host_dir, true}).second;
}

bool PathMap::unmount(std::string_view guest_point) {
    const std::string_view name = mount_name(guest_point);
    std::unique_lock lock(mounts_lock_);
    const auto it = mounts_.find(name);
    if (it == mounts_.end() || !it->second.writable) {
        return false;
    }
    mounts_.erase(it);
    return true;
}

Resolved PathMap::resolve(std::string_view guest, Access access) {
    if (guest.size() >= guest_path_max) {
        return fail(ENAMETOOLONG);
    }
    if (guest.empty() || guest[0] != '/') {
        log_refused("relative paths", "not absolute");
        return fail(ENOENT);
    }

    // Split into components, dropping empty ones and ".". The first is the mount point.
    std::vector<std::string_view> parts;
    for (std::size_t pos = 0; pos <= guest.size();) {
        std::size_t end = guest.find('/', pos);
        if (end == std::string_view::npos) {
            end = guest.size();
        }
        const std::string_view part = guest.substr(pos, end - pos);
        if (part.size() > guest_name_max) {
            return fail(ENAMETOOLONG);
        }
        if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        pos = end + 1;
    }
    if (parts.empty()) {
        log_refused("/", "no mount");
        return fail(ENOENT);
    }

    Mount mount;
    {
        std::shared_lock lock(mounts_lock_);
        const auto it = mounts_.find(parts[0]);
        if (it == mounts_.end()) {
            lock.unlock();
            log_refused("/" + std::string(parts[0]),
                        parts[0] == hostapp ? "debug output is not mapped" : "no mount");
            return fail(ENOENT);
        }
        mount = it->second;
    }

    // Apply ".." below the mount point. It never leaves the mount.
    std::vector<std::string_view> rest;
    for (std::size_t i = 1; i < parts.size(); ++i) {
        if (parts[i] != "..") {
            rest.push_back(parts[i]);
        } else if (rest.empty()) {
            return fail(ENOENT);
        } else {
            rest.pop_back();
        }
    }
    std::string relative;
    for (const std::string_view part : rest) {
        if (is_junk_name(part)) {
            return fail(ENOENT);
        }
        relative += relative.empty() ? "" : "/";
        relative += part;
    }

    if (access == Access::write && !mount.writable) {
        return fail(EROFS);
    }
    fs::path host = relative.empty() ? mount.host : mount.host / relative;
    // A trailing "/" or "/." names a directory: keep the "/" so the host call fails with ENOTDIR
    // on a file, as it does on the guest.
    if (!relative.empty() && (guest.ends_with('/') || guest.ends_with("/."))) {
        host += "/";
    }
    return {std::move(host), 0, std::string(parts[0]), relative.empty()};
}

void PathMap::log_refused(const std::string& first, std::string_view why) {
    std::lock_guard lock(log_lock_);
    if (logged_.insert(first).second) {
        common::log(common::LogLevel::warn, "hle.libkernel", "refusing guest paths under {} ({})",
                    first, why);
    }
}

PathMap& path_map() {
    static PathMap map;
    return map;
}

}  // namespace bb::hle::kernel
