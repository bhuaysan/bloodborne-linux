// libSceUserService: one user, logged in from the start, named after the profile.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>

#include "hle/hle.hpp"
#include "hle/system/internal.hpp"
#include "hle/system/log.hpp"
#include "hle/system/system.hpp"

namespace bb::hle::system {
namespace {

constexpr std::string_view library = "libSceUserService";

struct State {
    std::atomic<bool> initialized = false;
    std::mutex mutex;
    std::string name = "default";
};

State& state() {
    static State& s = *new State;  // leaked: game threads may call in during exit
    return s;
}

bool initialized() {
    return state().initialized.load();
}

}  // namespace

void set_user_name(std::string_view name) {
    State& s = state();
    const std::lock_guard lock(s.mutex);
    s.name = name;
}

std::string user_name() {
    State& s = state();
    const std::lock_guard lock(s.mutex);
    return s.name;
}

std::int32_t user_service_initialize(const void* /*params*/) {
    // The game initialises from two places, the second accepting "already initialised"
    // (0x20e1e69, 0xcba1ac).
    log(library, BB_SYSTEM_CALLER(), "initialize");
    return state().initialized.exchange(true) ? user_service_error_already_initialized : 0;
}

std::int32_t user_service_terminate() {
    log(library, BB_SYSTEM_CALLER(), "terminate");
    return state().initialized.exchange(false) ? 0 : user_service_error_not_initialized;
}

std::int32_t user_service_get_event(void* event) {
    // No user ever logs in or out. The game polls this until it fails (0x21628e9).
    if (!initialized()) {
        return user_service_error_not_initialized;
    }
    return event == nullptr ? user_service_bb_error_invalid_argument
                            : user_service_bb_error_no_event;
}

std::int32_t user_service_get_user_id(std::int32_t* id) {
    if (!initialized()) {
        return user_service_error_not_initialized;
    }
    if (id == nullptr) {
        return user_service_bb_error_invalid_argument;
    }
    *id = user_id;
    return 0;
}

std::int32_t user_service_get_user_list(std::int32_t* list) {
    if (!initialized()) {
        return user_service_error_not_initialized;
    }
    if (list == nullptr) {
        return user_service_bb_error_invalid_argument;
    }
    list[0] = user_id;
    std::fill(list + 1, list + user_list_size, no_user);
    return 0;
}

std::int32_t user_service_get_user_name(std::int32_t id, char* buf, std::size_t size) {
    if (!initialized()) {
        return user_service_error_not_initialized;
    }
    if (id != user_id || buf == nullptr || size == 0) {
        log(library, BB_SYSTEM_CALLER(), "user name: invalid arguments: id {} size {}", id, size);
        return user_service_bb_error_invalid_argument;
    }
    const std::string name = user_name();
    const std::size_t n = std::min(name.size(), size - 1);
    std::memcpy(buf, name.data(), n);
    buf[n] = '\0';
    return 0;
}

void reset_user_service() {
    State& s = state();
    s.initialized = false;
    set_user_name("default");
}

BB_HLE_FUNCTION("libSceUserService", "j3YMu1MVNNo", user_service_initialize);
BB_HLE_FUNCTION("libSceUserService", "bwFjS+bX9mA", user_service_terminate);
BB_HLE_FUNCTION("libSceUserService", "yH17Q6NWtVg", user_service_get_event);
// sceUserServiceGetInitialUser
BB_HLE_FUNCTION("libSceUserService", "CdWp0oHWGr0", user_service_get_user_id);
// sceUserServiceGetLoginUserIdList
BB_HLE_FUNCTION("libSceUserService", "fPhymKNvK-A", user_service_get_user_list);
// sceUserServiceGetUserName
BB_HLE_FUNCTION("libSceUserService", "1xxcMiGu2fo", user_service_get_user_name);

}  // namespace bb::hle::system
