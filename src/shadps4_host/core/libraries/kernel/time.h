// The time stamp counter video_core reads for GPU clock values (pm4_cmds.h, GetGpuPerfCounter).
#pragma once

#include "common/clock.hpp"
#include "common/types.h"

namespace Libraries::Kernel {

// Our libkernel's sceKernelReadTsc (src/hle/kernel/time.cpp) counts nanoseconds of the runtime's
// process clock, so the frequency is 1 GHz.
constexpr u64 sceKernelGetTscFrequency() { return 1'000'000'000; }

// The same clock as libkernel's sceKernelReadTsc, so GPU timestamps compare with the game's TSC.
inline u64 sceKernelReadTsc() { return bb::common::process_nsec(); }

}  // namespace Libraries::Kernel
