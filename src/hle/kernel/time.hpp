// Kernel time and sleep (docs/design/m3-core-os.md, "Time"). The wall clock comes from
// CLOCK_REALTIME, the monotonic clock from CLOCK_MONOTONIC, sleeps go through clock_nanosleep.
#pragma once

#include <cstdint>

#include "hle/kernel/abi.hpp"

namespace bb::hle::kernel {

// ---- main-loop probe ---------------------------------------------------------------------------

// Receives the return address of every libScePosix gettimeofday call: in game code, the
// instruction after the call.
using GettimeofdayObserver = void (*)(std::uintptr_t return_address);

// Installs `observer` (nullptr removes it) and returns the one it replaces. The observer runs on
// the calling thread, inside gettimeofday.
GettimeofdayObserver set_gettimeofday_observer(GettimeofdayObserver observer);

// ---- the HLE functions, declared for the tests -------------------------------------------------

// struct timezone of gettimeofday: minutes west of UTC in standard time, and non-zero while
// daylight saving time is in effect (eboot 0x1a3866).
struct GuestTimezone {
    std::int32_t minuteswest;
    std::int32_t dsttime;
};

// POSIX convention: 0, or -1 with the guest errno set.
int guest_clock_gettime(std::int32_t clock, GuestTimespec* out);
int guest_gettimeofday(GuestTimeval* tv, GuestTimezone* tz);  // libkernel
int posix_gettimeofday(GuestTimeval* tv, GuestTimezone* tz);  // libScePosix: tells the observer
int guest_nanosleep(const GuestTimespec* request, GuestTimespec* remain);
int guest_usleep(std::uint32_t microseconds);

// The unslept seconds, 0 when the sleep was not interrupted.
std::uint32_t guest_sleep(std::uint32_t seconds);

// sce convention: 0, or an sce error code.
std::int32_t sceKernelGettimeofday(GuestTimeval* tv);
std::int32_t sceKernelUsleep(std::uint32_t microseconds);

}  // namespace bb::hle::kernel
