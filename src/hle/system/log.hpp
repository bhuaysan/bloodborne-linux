// Logging for the system services, until the logger of m3-core-os.md ("Logging") exists.
#pragma once

#include <cstdio>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace bb::hle::system {

// Writes "hle.<library>: <message> (caller <address>)" to stderr. `caller` is the return address
// of the HLE function (BB_SYSTEM_CALLER()), a runtime address in the calling module.
template <class... Args>
void log(std::string_view library, const void* caller, std::format_string<Args...> format,
         Args&&... args) {
    const std::string line = std::format("hle.{}: {} (caller {})\n", library,
                                         std::format(format, std::forward<Args>(args)...), caller);
    std::fputs(line.c_str(), stderr);
}

}  // namespace bb::hle::system

// The return address of the current function: in game code, the instruction after its call.
#define BB_SYSTEM_CALLER() __builtin_return_address(0)
