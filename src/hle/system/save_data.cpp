// libSceSaveData and libSceSaveDataDialog (save_data.hpp, docs/formats/savedata.md).

#include "hle/system/save_data.hpp"

#include <array>
#include <cstring>
#include <filesystem>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include "common/runtime_dirs.hpp"
#include "hle/hle.hpp"
#include "hle/kernel/paths.hpp"
#include "hle/system/log.hpp"

namespace bb::hle::system {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view library = "libSceSaveData";

// Mount points we hand out: /savedata0, /savedata1, ... (our choice; the game only uses the
// string as a path prefix, 0x283ff5b).
constexpr unsigned max_mounts = 16;

struct State {
    std::mutex lock;
    std::optional<fs::path> root;
    std::array<std::string, max_mounts> mounted;  // directory name per mount point, "" if free
};

State& state() {
    static State* s = new State;  // leaked: guest threads may still call at exit
    return *s;
}

// A directory name from a 32-byte guest buffer: [A-Za-z0-9_-], 1 to 31 bytes, as the game's
// names are ("SPRJ" and four hex digits, 0x283e820). Anything else could leave the save root.
std::optional<std::string> dir_name_of(const char* buffer) {
    if (buffer == nullptr) {
        return std::nullopt;
    }
    const std::string name(buffer, strnlen(buffer, 32));
    if (name.empty() || name.size() > 31) {
        return std::nullopt;
    }
    for (const char c : name) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) {
            return std::nullopt;
        }
    }
    return name;
}

std::string mount_point_name(unsigned index) {
    return std::format("/savedata{}", index);
}

// The mount index of a mounted output record, or nullopt.
std::optional<unsigned> mounted_index(State& s, const SaveMountOutput* mounted) {
    if (mounted == nullptr) {
        return std::nullopt;
    }
    const std::string_view point(mounted->mount_point,
                                 strnlen(mounted->mount_point, sizeof mounted->mount_point));
    for (unsigned i = 0; i < max_mounts; ++i) {
        if (!s.mounted[i].empty() && point == mount_point_name(i)) {
            return i;
        }
    }
    return std::nullopt;
}

}  // namespace

fs::path save_data_root(const common::RuntimeDirs& dirs) {
    return dirs.ensure(dirs.profile_data() / "savedata");
}

void set_save_data_root(const fs::path& root) {
    State& s = state();
    const std::lock_guard guard(s.lock);
    s.root = root;
}

void reset_save_data() {
    State& s = state();
    const std::lock_guard guard(s.lock);
    for (unsigned i = 0; i < max_mounts; ++i) {
        if (!s.mounted[i].empty()) {
            kernel::path_map().unmount(mount_point_name(i));
            s.mounted[i].clear();
        }
    }
    s.root.reset();
}

std::int32_t save_data_initialize(const void* /*params*/) {
    log(library, BB_SYSTEM_CALLER(), "initialize");
    return save_ok;
}

std::int32_t save_data_terminate() {
    return save_ok;
}

std::int32_t save_data_mount(const SaveMountInput* in, SaveMountOutput* out) {
    State& s = state();
    const std::lock_guard guard(s.lock);
    if (in == nullptr || out == nullptr || !s.root) {
        return save_failed;
    }
    const std::optional<std::string> name = dir_name_of(in->dir_name);
    if (!name) {
        log(library, BB_SYSTEM_CALLER(), "mount: invalid directory name");
        return save_failed;
    }
    const fs::path dir = *s.root / *name;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        if ((in->mode & save_mode_create) == 0) {
            return save_not_found;
        }
        fs::create_directories(dir, ec);
        if (ec) {
            return save_failed;
        }
    }
    unsigned index = max_mounts;
    for (unsigned i = 0; i < max_mounts; ++i) {
        if (s.mounted[i] == *name) {
            return save_failed;  // already mounted; the game unmounts on every path
        }
        if (s.mounted[i].empty() && index == max_mounts) {
            index = i;
        }
    }
    if (index == max_mounts || !kernel::path_map().mount(mount_point_name(index), dir)) {
        return save_failed;
    }
    s.mounted[index] = *name;
    std::memset(out, 0, sizeof *out);
    const std::string point = mount_point_name(index);
    std::memcpy(out->mount_point, point.c_str(), point.size() + 1);
    log(library, BB_SYSTEM_CALLER(), "mount {} at {} (mode {:#x}, {} blocks)", *name, point,
        in->mode, in->blocks);
    return save_ok;
}

std::int32_t save_data_unmount(const SaveMountOutput* mounted) {
    State& s = state();
    const std::lock_guard guard(s.lock);
    const std::optional<unsigned> index = mounted_index(s, mounted);
    if (!index) {
        return save_failed;
    }
    kernel::path_map().unmount(mount_point_name(*index));
    s.mounted[*index].clear();
    return save_ok;
}

std::int32_t save_data_search(const SaveSearchCondition* cond, SaveSearchResult* result) {
    State& s = state();
    const std::lock_guard guard(s.lock);
    if (cond == nullptr || result == nullptr || !s.root) {
        return save_failed;
    }
    const std::optional<std::string> name = dir_name_of(cond->dir_name);
    if (!name) {
        return save_failed;
    }
    std::error_code ec;
    result->hits = 0;
    if (fs::is_directory(*s.root / *name, ec)) {
        if (result->capacity >= 1 && result->entries != nullptr) {
            std::memset(result->entries[0], 0, sizeof result->entries[0]);
            std::memcpy(result->entries[0], name->c_str(), name->size());
        }
        result->hits = 1;
    }
    return save_ok;
}

std::int32_t save_data_erase(const SaveEraseInput* in) {
    State& s = state();
    const std::lock_guard guard(s.lock);
    if (in == nullptr || !s.root) {
        return save_failed;
    }
    const std::optional<std::string> name = dir_name_of(in->dir_name);
    if (!name) {
        return save_failed;
    }
    for (const std::string& m : s.mounted) {
        if (m == *name) {
            return save_failed;
        }
    }
    const fs::path dir = *s.root / *name;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return save_not_found;
    }
    fs::remove_all(dir, ec);
    log(library, BB_SYSTEM_CALLER(), "erase {}", *name);
    return ec ? save_failed : save_ok;
}

BB_HLE_FUNCTION("libSceSaveData", "ZkZhskCPXFw", save_data_initialize);  // name unknown
BB_HLE_FUNCTION("libSceSaveData", "yKDy8S5yLA0", save_data_terminate);   // name unknown
BB_HLE_FUNCTION("libSceSaveData", "32HQAQdwM2o", save_data_mount);       // name unknown
BB_HLE_FUNCTION("libSceSaveData", "BMR4F-Uek3E", save_data_unmount);     // name unknown
BB_HLE_FUNCTION("libSceSaveData", "dyIhnXq-0SM", save_data_search);      // name unknown
BB_HLE_FUNCTION("libSceSaveData", "S1GkePI17zQ", save_data_erase);       // name unknown

}  // namespace bb::hle::system
