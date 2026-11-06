// libSceSaveData and libSceSaveDataDialog: save directories in the profile. Both libraries are in
// (or derived from) the exposure log of docs/clean-room.md, so everything here comes from the
// eboot's call sites only and no function has a Sony-style name; docs/formats/savedata.md has
// the facts and the addresses that show them.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace bb::common {
class RuntimeDirs;
}

namespace bb::hle::system {

// The mount call's input record (eboot 0x283fd70-0x283fe40, the same helper for load and save).
struct SaveMountInput {
    std::uint32_t user_id;
    std::uint32_t pad0;
    const char* short_name;  // up to 10 bytes, null in practice
    const char* dir_name;    // the save directory, a 32-byte buffer
    const char* label;       // a 65-byte buffer
    std::uint64_t blocks;
    std::uint32_t mode;
};
static_assert(offsetof(SaveMountInput, dir_name) == 0x10 && offsetof(SaveMountInput, mode) == 0x28);

// The mount call's output record: the game uses the string at offset 0 as the path prefix of
// its files and passes the record back to unmount (0x283ff5b, 0x284001c). 0x40 bytes.
struct SaveMountOutput {
    char mount_point[16];
    std::uint8_t rest[0x30];
};
static_assert(sizeof(SaveMountOutput) == 0x40);

// The directory search's condition and result records (0x283e3b0).
struct SaveSearchCondition {
    std::uint32_t user_id;
    std::uint32_t pad0;
    const void* unused;
    const char* dir_name;
    std::uint32_t unknown_18;
    std::uint32_t unknown_1c;
};
struct SaveSearchResult {
    std::uint32_t hits;
    std::uint32_t pad0;
    char (*entries)[32];
    std::uint32_t capacity;
};
static_assert(offsetof(SaveSearchCondition, dir_name) == 0x10);
static_assert(offsetof(SaveSearchResult, capacity) == 0x10);

// The erase call's record (0x283e765): user id at 0, the directory name at 0x10.
struct SaveEraseInput {
    std::uint32_t user_id;
    std::uint32_t pad0;
    const void* unused;
    const char* dir_name;
};

// Results the game tells apart (its translation table at 0x283ee10).
inline constexpr std::int32_t save_ok = 0;
// The directory doesn't exist: "no save yet" on load (game result 6, 0x283ee10), a retry with
// mode bit 4 on save (0x2842481).
inline constexpr std::int32_t save_not_found = static_cast<std::int32_t>(0x809f0008u);
// Any other failure. Our choice: the code doesn't tell 0x809f0000-0x809f0007 apart (all give
// the game's generic error 8, 0x283ee10).
inline constexpr std::int32_t save_failed = static_cast<std::int32_t>(0x809f0001u);
// Mode bit that lets a mount create a missing directory (0x2842481 sets it after
// save_not_found, then mounts again).
inline constexpr std::uint32_t save_mode_create = 4;

// Where save directories live: profile_data()/savedata, created
// with mode 0700. bb-run passes it to set_save_data_root at start-up; until then every mount
// fails. A directory the game names SPRJ0000 is <root>/SPRJ0000/.
std::filesystem::path save_data_root(const common::RuntimeDirs& dirs);
void set_save_data_root(const std::filesystem::path& root);
// Unmounts everything and forgets the root (tests).
void reset_save_data();

std::int32_t save_data_initialize(const void* params);
std::int32_t save_data_terminate();
std::int32_t save_data_mount(const SaveMountInput* in, SaveMountOutput* out);
std::int32_t save_data_unmount(const SaveMountOutput* mounted);
std::int32_t save_data_search(const SaveSearchCondition* cond, SaveSearchResult* result);
std::int32_t save_data_erase(const SaveEraseInput* in);
}  // namespace bb::hle::system
