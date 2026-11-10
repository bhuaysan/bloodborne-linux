// The piece of shadPS4's libkernel process header that src/shadps4 uses.
#pragma once

#include "common/types.h"

namespace Libraries::Kernel {

// We run the base PS4 game, not the PS4 Pro one, so this is always false (0). It selects the base
// console's tiling tables and performance counters in video_core.
constexpr s32 sceKernelIsNeoMode() { return 0; }

}  // namespace Libraries::Kernel
