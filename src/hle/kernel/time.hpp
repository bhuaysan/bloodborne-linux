// Kernel time and sleep (docs/design/m3-core-os.md, "Time"). The wall clock comes from
// CLOCK_REALTIME, every counter from CLOCK_MONOTONIC, sleeps go through clock_nanosleep. The units
// of the imports that only symbols/nids.csv names, with their call sites, are in
// docs/formats/kernel-imports.md.
#pragma once

#include <cstdint>

#include "hle/kernel/abi.hpp"

namespace bb::hle::kernel {

// ---- test seam ---------------------------------------------------------------------------------

// The wall clock that the gettimeofday imports (libkernel, libScePosix, sceKernelGettimeofday)
// read, for differential tests of game code that reads the time. nullptr (the default) is the
// host's CLOCK_REALTIME.
using WallClock = GuestTimeval (*)();

// Installs `clock` (nullptr restores the host's) and returns the one it replaces.
WallClock set_wall_clock(WallClock clock);

// Sleeps until the wall clock that gettimeofday reads shows `deadline_us` (microseconds since the
// epoch), or returns at once when it already does. Host code that waits on the guest's wall
// clock sleeps with it (the frame pacer, #656). With a test clock installed it returns at once:
// a test clock has no host time to sleep against, and its reads stay the caller's.
void sleep_until_wall_clock(std::uint64_t deadline_us);

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
int guest_gettimeofday(GuestTimeval* tv, GuestTimezone* tz);  // libkernel and libScePosix
int guest_nanosleep(const GuestTimespec* request, GuestTimespec* remain);
int guest_usleep(std::uint32_t microseconds);
int guest_getrusage(std::int32_t who, GuestRusage* out);  // who 0 (RUSAGE_SELF) only

// The unslept seconds, 0 when the sleep was not interrupted.
std::uint32_t guest_sleep(std::uint32_t seconds);

// sce convention: 0, or an sce error code.
std::int32_t sceKernelGettimeofday(GuestTimeval* tv);
std::int32_t sceKernelUsleep(std::uint32_t microseconds);

// Libkernel imports whose names are only in symbols/nids.csv (docs/formats/kernel-imports.md).

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
