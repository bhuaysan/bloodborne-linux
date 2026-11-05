// The system services the start-up path needs (docs/design/m3-core-os.md, "System services"):
// libSceSysmodule, libSceUserService, libSceSystemService, libSceAppContent, libScePlayGo and
// libSceDiscMap.
//
// Every function is registered by NID in its source file. NIDs without a matched name in
// symbols/nids.csv have a descriptive name here and a comment that says so (m3-core-os.md,
// "Naming"). Values chosen for the game are documented next to the code with the call-site link
// address in the eboot (or libSceFios2, where named).
//
// Error codes: where the game compares a result with a constant, that constant is used. Where
// callers only test for non-zero, the value is ours (*_bb_error_*): in the user service's range
// for libSceUserService (whose range the game shows), otherwise in 0x80ff0000-0x80ffffff.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace bb::hle::system {

// Resets the state of every module (loaded ids, initialisation flags, user name) to the state at
// start. For tests.
void reset();

// --- libSceSysmodule ------------------------------------------------------------------------

// The game unloads ids it may not have loaded and accepts this result (0xc8733e).
inline constexpr std::int32_t sysmodule_error_not_loaded = static_cast<std::int32_t>(0x805a1001);

// The library the game uses after loading `id` (docs/formats/imports.md), or "unknown".
std::string_view sysmodule_library(std::uint32_t id);

std::int32_t sysmodule_load_module(std::uint32_t id);
std::int32_t sysmodule_unload_module(std::uint32_t id);
// 0 when `id` is loaded, sysmodule_error_not_loaded otherwise (0x2903d1c, 0x2903fc0).
std::int32_t sysmodule_is_loaded(std::uint32_t id);

// --- libSceUserService ----------------------------------------------------------------------

// Compared by the game: a second Initialize (0xcba1b5) and a query before Initialize (0xd1b351).
inline constexpr std::int32_t user_service_error_not_initialized =
    static_cast<std::int32_t>(0x80960002);
inline constexpr std::int32_t user_service_error_already_initialized =
    static_cast<std::int32_t>(0x80960003);
// Ours: callers only test for non-zero (GetEvent 0x21628e9, user name 0xd1be0a).
inline constexpr std::int32_t user_service_bb_error_no_event =
    static_cast<std::int32_t>(0x809600f0);
inline constexpr std::int32_t user_service_bb_error_invalid_argument =
    static_cast<std::int32_t>(0x809600f1);

// The one user that is logged in. The game treats -1 as "no user" (0x2161bd7), anything else as
// an id it passes back.
inline constexpr std::int32_t user_id = 1;
inline constexpr std::int32_t no_user = -1;
// Entries of the logged-in user list (the game reads four, 0x2161bc9).
inline constexpr std::size_t user_list_size = 4;

// The user name the user-name query returns: the profile name. Default "default".
void set_user_name(std::string_view name);
std::string user_name();

std::int32_t user_service_initialize(const void* params);
std::int32_t user_service_terminate();
std::int32_t user_service_get_event(void* event);
// Name unknown (NID CdWp0oHWGr0): writes the id of the user to *id.
std::int32_t user_service_get_user_id(std::int32_t* id);
// Name unknown (NID fPhymKNvK-A): writes the logged-in users to list[0..3], -1 for empty.
std::int32_t user_service_get_user_list(std::int32_t* list);
// Name unknown (NID 1xxcMiGu2fo): writes the user's name to buf, cut to size - 1 bytes and
// NUL-terminated (the game passes 0x11, 0x2161cd0).
std::int32_t user_service_get_user_name(std::int32_t id, char* buf, std::size_t size);

// --- libSceSystemService --------------------------------------------------------------------

// Ours: callers only test for non-zero (0x178a438, 0x215e756).
inline constexpr std::int32_t system_service_bb_error_no_event =
    static_cast<std::int32_t>(0x80ff0101);
inline constexpr std::int32_t system_service_bb_error_unknown_id =
    static_cast<std::int32_t>(0x80ff0102);
inline constexpr std::int32_t system_service_bb_error_invalid_argument =
    static_cast<std::int32_t>(0x80ff0103);

// The integer the query returns for id 1. The game maps it through the table at 0x4736630 to its
// own language index, 8 for 1, and index 8 is "engUS" in the game's list at 0x49bba84 (UTF-16).
inline constexpr std::int32_t system_language_english_us = 1;

// Writes the number of pending events (0) to the int at status + 0, which is all the game reads
// (0x178a412, 0x1e76a83). The rest of the caller's buffer is left alone.
std::int32_t system_service_get_status(void* status);
std::int32_t system_service_receive_event(void* event);
// Name unknown (NID fZo48un7LK4): an integer by id. The game asks only for id 1 (nine sites,
// e.g. 0x14ef12e), and maps the value through a 21-entry table.
std::int32_t system_service_get_int(std::int32_t id, std::int32_t* value);
// Name unknown (NID -+3hY+y8bNo): takes a URL (0x2037030 passes a web page of the game) and an
// int. Logged, success.
std::int32_t system_service_open_url(const char* url, std::int32_t arg);
// Name unknown (NID Vo5V8KAwCmk): no arguments, result ignored (0x19d4632, 0x2036fc0). Logged,
// success.
std::int32_t system_service_notify(void);

// --- libSceAppContent -----------------------------------------------------------------------

// Ours: the game ignores the result of every call (0x20261e6 to 0x202622a).
inline constexpr std::int32_t app_content_bb_error_unknown_key =
    static_cast<std::int32_t>(0x80ff0201);
inline constexpr std::int32_t app_content_bb_error_invalid_argument =
    static_cast<std::int32_t>(0x80ff0202);

// The number of keys the game asks for: 0 to 4, at 0x20261e6 to 0x202622a.
inline constexpr std::int32_t app_param_keys = 5;

std::int32_t app_content_initialize(const void* init_params, void* boot_params);
// Name unknown (NID 99b82IKXpH4): an integer by key (app_param_value).
std::int32_t app_content_get_int(std::int32_t key, std::int32_t* value);

// The values of keys 0 to 4.
// - Key 1: the only output the game reads. Its default before the call is 10 (data at
//   0x5128a00), and the game reads it as two decimal digits (0x20264d0, 0x2026500). We return
//   13, the one user-defined integer in the dump's sce_sys/param.sfo (USER_DEFINED_PARAM_1),
//   which has the same two-digit form. That the key names this parameter is our inference.
// - Keys 0, 2, 3, 4: 0. Nothing in the eboot reads their outputs (0x56a0248, 0x56a024c,
//   0x56a0250, 0x56a0254 are referenced only by the calls).
inline constexpr std::int32_t app_param_value[app_param_keys] = {0, 13, 0, 0, 0};

// Name unknown (NID xnd8BJzAxmk): writes the number of add-on content items to *count (0). The
// game passes (0, list, list size, count) and only reads the list when the count is non-zero
// (0x1fb67cf, 0x1fb6818).
std::int32_t app_content_get_addcont_count(std::uint32_t arg, void* list, std::uint32_t list_size,
                                           std::uint32_t* count);

// --- libScePlayGo ---------------------------------------------------------------------------

// Ours: the game tests only for non-zero (0x1fdeae9, 0x1fde8db).
inline constexpr std::int32_t playgo_bb_error_bad_handle = static_cast<std::int32_t>(0x80ff0301);
inline constexpr std::int32_t playgo_bb_error_invalid_argument =
    static_cast<std::int32_t>(0x80ff0302);

// The handle Open hands out.
inline constexpr std::int32_t playgo_handle = 1;
// The one chunk we report.
inline constexpr std::uint16_t playgo_chunk_id = 0;
// The per-chunk state byte we report. The game folds the bytes into its own state, starting at 2:
// a byte of 0 sets 0, a byte of 2 sets 1, any other value keeps the state (init 0x1fdeb40 to
// 0x1fdeb6d, per-frame poll 0x1fde8ea to 0x1fde90d). Only state 2 means done: init then skips
// the progress callback (0x1fdeb77) and the poll stops (0x1fde917); no chunks at all also give
// state 2 (0x1fdeb94, 0x1fde91b). So the byte must be neither 0 nor 2.
inline constexpr std::int8_t playgo_chunk_installed = 3;

std::int32_t playgo_initialize(const void* params);
std::int32_t playgo_open(std::int32_t* handle, const void* params);
// Name unknown (NID 73fF1MFU8hA): writes up to `max` chunk ids and their number (the game passes
// room for 100, 0x1fdeb04).
std::int32_t playgo_get_chunk_ids(std::int32_t handle, std::uint16_t* ids, std::uint32_t max,
                                  std::uint32_t* count);
// Name unknown (NID uWIYLFkkwqk): writes one state byte per chunk id (0x1fdeb30).
std::int32_t playgo_get_chunk_states(std::int32_t handle, const std::uint16_t* ids,
                                     std::uint32_t count, std::int8_t* states);
// Name unknown (NID 4AAcTU9R3XM): the game passes a mode 0, 1 or 2 (0x1fdec10). Logged, success.
std::int32_t playgo_set_mode(std::int32_t handle, std::int32_t mode);

// --- libSceDiscMap (imported by libSceFios2 only) -------------------------------------------

// Ours: libSceFios2 tests only for non-zero (libSceFios2 0x29c74, 0x2a386).
inline constexpr std::int32_t disc_map_bb_error_not_mapped = static_cast<std::int32_t>(0x80ff0401);

// Name unknown (NID lbQKqsERhtE): (path, ?, ?, int* out). libSceFios2 (0x2d721 to 0x2d73e)
// reports "remapped" only for result 0 with *out != 0, so we return 0 with *out = 0.
std::int32_t disc_map_is_remapped(const char* path, std::uint64_t arg1, std::uint64_t arg2,
                                  std::int32_t* out);
// Name unknown (NID ioKMruft1ek): (path, offset, size, out, out, out). For a non-zero result
// libSceFios2 keeps the original offset (tagged with bit 63, 0x29c78), for 0 it takes the second
// output shifted by 16 (0x29c98). We return disc_map_bb_error_not_mapped and write nothing.
std::int32_t disc_map_map_range(const char* path, std::uint64_t offset, std::uint64_t size,
                                std::uint64_t* out1, std::uint64_t* out2, std::uint64_t* out3);

}  // namespace bb::hle::system
