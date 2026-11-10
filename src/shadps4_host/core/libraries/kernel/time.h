// The time stamp counter video_core reads for GPU clock values (pm4_cmds.h, GetGpuPerfCounter).
#pragma once

#include <ctime>

#include "common/types.h"

namespace Libraries::Kernel {

// Our libkernel counts its time stamp counter in nanoseconds of CLOCK_MONOTONIC
// (src/hle/kernel/time.cpp, sceKernelReadTsc), so the frequency is 1 GHz.
constexpr u64 sceKernelGetTscFrequency() { return 1'000'000'000; }

// CLOCK_MONOTONIC in nanoseconds. libkernel's counter starts at the runtime's start instead; only
// differences between GPU clock values have a meaning, and those agree.
inline u64 sceKernelReadTsc() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<u64>(now.tv_sec) * 1'000'000'000 + static_cast<u64>(now.tv_nsec);
}

}  // namespace Libraries::Kernel
