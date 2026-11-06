// libSceSaveData and libSceSaveDataDialog (save_data.hpp, docs/formats/savedata.md).

#include "hle/system/save_data.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
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
constexpr std::string_view dialog_library = "libSceSaveDataDialog";

// Mount points we hand out: /savedata0, /savedata1, ... (our choice; the game only uses the
// string as a path prefix, 0x283ff5b).
constexpr unsigned max_mounts = 16;
// The byte store: the game declares at most 0x100000 bytes (0x283f040). Our bound for reads and
// writes, with room to spare.
constexpr std::uint64_t store_limit = 0x400000;

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

fs::path store_path(const fs::path& root, std::uint32_t user_id) {
    return root / std::format("store-{}.bin", user_id);
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

std::int32_t save_data_set_text(const SaveMountOutput* mounted, std::int32_t kind,
                                const char* text, std::uint32_t size) {
    State& s = state();
    const std::lock_guard guard(s.lock);
    if (!mounted_index(s, mounted) || text == nullptr) {
        return save_failed;
    }
    // Our choice: the text is kept nowhere (the eboot doesn't show who reads it).
    log(library, BB_SYSTEM_CALLER(), "text {}: {}", kind, std::string_view(text, strnlen(text, size)));
    return save_ok;
}

std::int32_t save_data_set_blob(const SaveMountOutput* mounted, const void* record) {
    State& s = state();
    const std::lock_guard guard(s.lock);
    return mounted_index(s, mounted) && record != nullptr ? save_ok : save_failed;
}

std::int32_t save_store_read(std::uint32_t user_id, void* buffer, std::uint64_t size,
                             std::uint64_t offset) {
    State& s = state();
    const std::lock_guard guard(s.lock);
    if (buffer == nullptr || !s.root || offset > store_limit || size > store_limit - offset) {
        return save_failed;
    }
    // Bytes the store doesn't hold read as zeros, so a missing store looks empty: the game
    // then finds no header and reports "no save yet" (0x2841056).
    std::memset(buffer, 0, size);
    const int fd = ::open(store_path(*s.root, user_id).c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return errno == ENOENT ? save_ok : save_failed;
    }
    std::uint64_t done = 0;
    while (done < size) {
        const ssize_t n = ::pread(fd, static_cast<char*>(buffer) + done, size - done,
                                  static_cast<off_t>(offset + done));
        if (n <= 0) {
            break;
        }
        done += static_cast<std::uint64_t>(n);
    }
    ::close(fd);
    return save_ok;
}

std::int32_t save_store_write(std::uint32_t user_id, const void* buffer, std::uint64_t size,
                              std::uint64_t offset) {
    State& s = state();
    const std::lock_guard guard(s.lock);
    if (buffer == nullptr || !s.root || offset > store_limit || size > store_limit - offset) {
        return save_failed;
    }
    const int fd = ::open(store_path(*s.root, user_id).c_str(), O_WRONLY | O_CREAT | O_CLOEXEC,
                          0600);
    if (fd < 0) {
        return save_failed;
    }
    std::uint64_t done = 0;
    bool ok = true;
    while (done < size) {
        const ssize_t n = ::pwrite(fd, static_cast<const char*>(buffer) + done, size - done,
                                   static_cast<off_t>(offset + done));
        if (n <= 0) {
            ok = false;
            break;
        }
        done += static_cast<std::uint64_t>(n);
    }
    ok = ::close(fd) == 0 && ok;
    return ok ? save_ok : save_failed;
}

std::int32_t save_store_declare(std::uint32_t user_id, std::uint64_t size, const void* record) {
    State& s = state();
    const std::lock_guard guard(s.lock);
    if (record == nullptr || !s.root || size > store_limit) {
        return save_failed;
    }
    log(library, BB_SYSTEM_CALLER(), "store for user {}: {:#x} bytes", user_id, size);
    return save_ok;
}

// The dialog is only reached when a save mount fails with 0x809f000a (0x2841ca0), which we
// never return. Opening it fails, so the game goes straight to closing it (0x28432f5).
std::int32_t save_dialog_initialize() {
    return save_ok;
}

std::int32_t save_dialog_open(const void* /*params*/) {
    log(dialog_library, BB_SYSTEM_CALLER(), "open: no dialog");
    return save_failed;
}

std::int32_t save_dialog_status() {
    return 0;  // anything but 2 ends the game's poll (0x284330a)
}

std::int32_t save_dialog_close() {
    return save_ok;
}

// sceSaveDataInitialize
BB_HLE_FUNCTION("libSceSaveData", "ZkZhskCPXFw", save_data_initialize);
// sceSaveDataTerminate
BB_HLE_FUNCTION("libSceSaveData", "yKDy8S5yLA0", save_data_terminate);
// sceSaveDataMount
BB_HLE_FUNCTION("libSceSaveData", "32HQAQdwM2o", save_data_mount);
// sceSaveDataUmount
BB_HLE_FUNCTION("libSceSaveData", "BMR4F-Uek3E", save_data_unmount);
// sceSaveDataDirNameSearch
BB_HLE_FUNCTION("libSceSaveData", "dyIhnXq-0SM", save_data_search);
// sceSaveDataDelete
BB_HLE_FUNCTION("libSceSaveData", "S1GkePI17zQ", save_data_erase);
// sceSaveDataSetParam
BB_HLE_FUNCTION("libSceSaveData", "85zul--eGXs", save_data_set_text);
// sceSaveDataSaveIcon
BB_HLE_FUNCTION("libSceSaveData", "c88Yy54Mx0w", save_data_set_blob);
// sceSaveDataGetSaveDataMemory
BB_HLE_FUNCTION("libSceSaveData", "7Bt5pBC-Aco", save_store_read);
// sceSaveDataSetSaveDataMemory
BB_HLE_FUNCTION("libSceSaveData", "h3YURzXGSVQ", save_store_write);
// sceSaveDataSetupSaveDataMemory
BB_HLE_FUNCTION("libSceSaveData", "v7AAAMo0Lz4", save_store_declare);
// sceSaveDataDialogInitialize
BB_HLE_FUNCTION("libSceSaveDataDialog", "s9e3+YpRnzw", save_dialog_initialize);
// sceSaveDataDialogOpen
BB_HLE_FUNCTION("libSceSaveDataDialog", "4tPhsP6FpDI", save_dialog_open);
// sceSaveDataDialogUpdateStatus
BB_HLE_FUNCTION("libSceSaveDataDialog", "KK3Bdg1RWK0", save_dialog_status);
// sceSaveDataDialogTerminate
BB_HLE_FUNCTION("libSceSaveDataDialog", "YuH2FA7azqQ", save_dialog_close);

}  // namespace bb::hle::system
