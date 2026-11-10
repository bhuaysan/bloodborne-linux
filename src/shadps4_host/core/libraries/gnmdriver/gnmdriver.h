// The piece of shadPS4's GNM driver header that video_core uses.
#pragma once

#include "common/types.h"

namespace Libraries::GnmDriver {

// The base PS4's GPU core clock, 800 MHz, as upstream returns it without Neo mode.
constexpr u32 sceGnmGetGpuCoreClockFrequency() { return 800'000'000; }

}  // namespace Libraries::GnmDriver
