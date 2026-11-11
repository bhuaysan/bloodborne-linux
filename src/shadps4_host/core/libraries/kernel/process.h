// The pieces of shadPS4's libkernel process header that src/shadps4 uses.
#pragma once

#include "common/types.h"

namespace Libraries::Kernel {

// We run the base PS4 game, not the PS4 Pro one, so this is always false (0). It selects the base
// console's tiling tables and performance counters in video_core.
constexpr s32 sceKernelIsNeoMode() { return 0; }

// The SDK version the eboot was built with: sdk_version of its PT_SCE_PROCPARAM block (2.00).
// AvPlayer and ImeDialog read it.
inline constexpr s32 compiled_sdk_version = 0x02000071;

// Writes compiled_sdk_version to *ver. Returns 0, or libkernel's EINVAL error for null.
s32 sceKernelGetCompiledSdkVersion(s32* ver);

}  // namespace Libraries::Kernel
