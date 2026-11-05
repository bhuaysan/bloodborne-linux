// The system services the start-up path needs (docs/design/m3-core-os.md, "System services"):
// libSceSysmodule and libSceUserService.
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

}  // namespace bb::hle::system
