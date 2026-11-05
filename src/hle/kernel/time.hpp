// Kernel time and sleep (docs/design/m3-core-os.md, "Time"). The wall clock comes from
// CLOCK_REALTIME, every counter from CLOCK_MONOTONIC, sleeps go through clock_nanosleep. The units
// of the unnamed imports, with their call sites, are in docs/formats/kernel-imports.md.
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

// struct rusage, 144 bytes. Only the two times are filled (libc reads nothing else); the other
// 14 fields are 0.
struct GuestRusage {
    GuestTimeval utime;
    GuestTimeval stime;
    std::int64_t other[14];
};

// POSIX convention: 0, or -1 with the guest errno set.
int guest_clock_gettime(std::int32_t clock, GuestTimespec* out);
int guest_gettimeofday(GuestTimeval* tv, GuestTimezone* tz);  // libkernel
int posix_gettimeofday(GuestTimeval* tv, GuestTimezone* tz);  // libScePosix: tells the observer
int guest_nanosleep(const GuestTimespec* request, GuestTimespec* remain);
int guest_usleep(std::uint32_t microseconds);
int guest_getrusage(std::int32_t who, GuestRusage* out);  // who 0 (RUSAGE_SELF) only

// The unslept seconds, 0 when the sleep was not interrupted.
std::uint32_t guest_sleep(std::uint32_t seconds);

// sce convention: 0, or an sce error code.
std::int32_t sceKernelGettimeofday(GuestTimeval* tv);
std::int32_t sceKernelUsleep(std::uint32_t microseconds);

// Unnamed libkernel imports (names unknown, see docs/formats/kernel-imports.md).

// 4J2sUJmuHZQ: microseconds since the runtime started.
std::uint64_t process_time_usec();

// -2IRUCO--PM: nanoseconds since the runtime started.
std::uint64_t process_counter_nsec();

// QBi7HCK03hw: clock_gettime with the sce convention.
std::int32_t clock_gettime_sce(std::int32_t clock, GuestTimespec* out);

// kOcnerypnQA: out[0] = the host's offset east of UTC in standard time, out[1] = 60 while daylight
// saving time is in effect, else 0, both in minutes. Returns 0.
std::int32_t local_time_offsets(std::int32_t out[2]);

}  // namespace bb::hle::kernel
