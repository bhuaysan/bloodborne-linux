// The piece of shadPS4's libkernel header that ImeDialog uses: the system software version.
#pragma once

#include "common/types.h"

namespace Libraries::Kernel {

// The layout of libkernel's sceKernelGetSystemSwVersion result.
struct SwVersionStruct {
    u64 struct_size;
    char text_representation[0x1c];
    u32 hex_representation;
};

// The version upstream reports (CURRENT_FIRMWARE_VERSION of v.0.21.0), so ImeDialog picks the
// same options as in the reference run.
inline constexpr u32 system_sw_version = 0x14008001;

// Fills `ret` with system_sw_version ("14.008.001" as text). Returns 0, also for null.
s32 sceKernelGetSystemSwVersion(SwVersionStruct* ret);

}  // namespace Libraries::Kernel
