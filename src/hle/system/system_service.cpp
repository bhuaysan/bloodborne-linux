// libSceSystemService: no system events, fixed integer settings.

#include <cstdint>
#include <cstring>
#include <string_view>

#include "hle/hle.hpp"
#include "hle/system/log.hpp"
#include "hle/system/system.hpp"

namespace bb::hle::system {
namespace {

constexpr std::string_view library = "libSceSystemService";

// The ids the game asks for, with our values (see system.hpp).
struct IntSetting {
    std::int32_t id, value;
};
constexpr IntSetting int_settings[] = {
    {1, system_language_english_us},  // 0x14ef12e and eight more sites
};

}  // namespace

std::int32_t system_service_get_status(void* status) {
    if (status == nullptr) {
        return system_service_bb_error_invalid_argument;
    }
    const std::int32_t events = 0;
    std::memcpy(status, &events, sizeof events);
    return 0;
}

std::int32_t system_service_receive_event(void* event) {
    // Called only while the status reports events (0x178a419), so never in practice.
    log(library, BB_SYSTEM_CALLER(), "receive event: none");
    return event == nullptr ? system_service_bb_error_invalid_argument
                            : system_service_bb_error_no_event;
}

std::int32_t system_service_get_int(std::int32_t id, std::int32_t* value) {
    if (value == nullptr) {
        return system_service_bb_error_invalid_argument;
    }
    for (const IntSetting& s : int_settings) {
        if (s.id == id) {
            *value = s.value;
            return 0;
        }
    }
    // Callers that ignore the result keep their -1 (0x2036fe7) or check it (0x215e756).
    log(library, BB_SYSTEM_CALLER(), "integer query: unknown id {}", id);
    return system_service_bb_error_unknown_id;
}

std::int32_t system_service_open_url(const char* url, std::int32_t arg) {
    log(library, BB_SYSTEM_CALLER(), "open url \"{}\" {}: ignored",
        url != nullptr ? url : "(null)", arg);
    return 0;
}

std::int32_t system_service_notify(void) {
    log(library, BB_SYSTEM_CALLER(), "Vo5V8KAwCmk: ignored");
    return 0;
}

BB_HLE_FUNCTION("libSceSystemService", "rPo6tV8D9bM", system_service_get_status);
BB_HLE_FUNCTION("libSceSystemService", "656LMQSrg6U", system_service_receive_event);
// sceSystemServiceParamGetInt
BB_HLE_FUNCTION("libSceSystemService", "fZo48un7LK4", system_service_get_int);
// sceSystemServiceLaunchWebBrowser
BB_HLE_FUNCTION("libSceSystemService", "-+3hY+y8bNo", system_service_open_url);
// sceSystemServiceHideSplashScreen
BB_HLE_FUNCTION("libSceSystemService", "Vo5V8KAwCmk", system_service_notify);

}  // namespace bb::hle::system
