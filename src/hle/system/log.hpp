// Logging for the system services, through the runtime's logger (common/log.hpp).
#pragma once

#include <format>
#include <string>
#include <string_view>
#include <utility>

#include "common/log.hpp"

namespace bb::hle::system {

// Logs "<message> (caller <address>)" at info on the channel "hle.<library>". `caller` is the
// return address of the HLE function (BB_SYSTEM_CALLER()), a runtime address in the calling
// module.
template <class... Args>
void log(std::string_view library, const void* caller, std::format_string<Args...> format,
         Args&&... args) {
    const std::string channel = std::format("hle.{}", library);
    if (common::log_enabled(common::LogLevel::info, channel)) {
        common::log(common::LogLevel::info, channel, "{} (caller {})",
                    std::format(format, std::forward<Args>(args)...), caller);
    }
}

}  // namespace bb::hle::system

// The return address of the current function: in game code, the instruction after its call.
#define BB_SYSTEM_CALLER() __builtin_return_address(0)
