#include "hle/kernel/paths.hpp"

#include <cerrno>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "common/runtime_dirs.hpp"
#include "hle/kernel/abi.hpp"

namespace bb::hle::kernel {

namespace fs = std::filesystem;

namespace {

// The guest's PATH_MAX (including the NUL) and NAME_MAX, docs/formats/kernel-abi.md.
constexpr std::size_t guest_path_max = 1024;
constexpr std::size_t guest_name_max = 255;

// Guest debug output, /hostapp/%s in the eboot (0x47e412c). It has no host counterpart.
constexpr std::string_view hostapp = "hostapp";

Resolved fail(int host_errno) {
    return {{}, guest_errno(host_errno).value()};
}

char lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool equal_ignoring_case(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

bool host_exists(const fs::path& path) {
    std::error_code ec;
    return fs::symlink_status(path, ec).type() != fs::file_type::not_found && !ec;
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
    {
        std::unique_lock lock(mounts_lock_);
        mounts_[std::string(game_mount.substr(1))] = Mount{game_dir, false};
    }
    std::lock_guard lock(cache_lock_);
    case_cache_.clear();
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
    if (!mount.writable && !relative.empty() && !host_exists(host)) {
        host = find_case_insensitive(mount.host, relative);
    }
    return {std::move(host), 0};
}

// Walks `relative` below `root` one component at a time. A component that doesn't exist exactly
// is replaced by the first directory entry (in name order) that matches it ignoring ASCII case.
// The first component without a match ends the walk; it and the rest are appended as given.
fs::path PathMap::find_case_insensitive(const fs::path& root, const std::string& relative) {
    {
        std::lock_guard lock(cache_lock_);
        if (const auto it = case_cache_.find(relative); it != case_cache_.end()) {
            return it->second;
        }
    }
    fs::path current = root;
    bool found = true;
    for (const fs::path& part : fs::path(relative)) {
        if (found && !host_exists(current / part)) {
            std::string best;
            std::error_code ec;
            for (fs::directory_iterator it(current, ec), end; !ec && it != end;
                 it.increment(ec)) {
                const std::string name = it->path().filename().string();
                if (!is_junk_name(name) && equal_ignoring_case(name, part.string()) &&
                    (best.empty() || name < best)) {
                    best = name;
                }
            }
            if (best.empty()) {
                found = false;
            } else {
                current /= best;
                continue;
            }
        }
        current /= part;
    }
    if (found) {
        std::lock_guard lock(cache_lock_);
        if (case_cache_.emplace(relative, current).second) {
            std::fprintf(stderr, "hle.kernel: %s/%s found as %s (case-insensitive)\n",
                         game_mount.data(), relative.c_str(), current.c_str());
        }
    }
    return current;
}

void PathMap::log_refused(const std::string& first, std::string_view why) {
    std::lock_guard lock(log_lock_);
    if (logged_.insert(first).second) {
        std::fprintf(stderr, "hle.kernel: refusing guest paths under %s (%.*s)\n", first.c_str(),
                     static_cast<int>(why.size()), why.data());
    }
}

PathMap& path_map() {
    static PathMap map;
    return map;
}

}  // namespace bb::hle::kernel
