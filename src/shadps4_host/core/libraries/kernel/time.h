// The time stamp counter video_core reads for GPU clock values (pm4_cmds.h, GetGpuPerfCounter),
// and the process time of the system libraries.
#pragma once

#include "common/clock.hpp"
#include "common/types.h"

namespace Libraries::Kernel {

// Our libkernel's sceKernelReadTsc (src/hle/kernel/time.cpp) counts nanoseconds of the runtime's
// process clock, so the frequency is 1 GHz.
constexpr u64 sceKernelGetTscFrequency() { return 1'000'000'000; }

// The same clock as libkernel's sceKernelReadTsc, so GPU timestamps compare with the game's TSC.
inline u64 sceKernelReadTsc() { return bb::common::process_nsec(); }

// Our libkernel's process time in microseconds (src/hle/kernel/time.cpp), which the game reads
// too. Defined by the system libraries' host (syslibs/kernel.cpp).
u64 sceKernelGetProcessTime();

}  // namespace Libraries::Kernel
