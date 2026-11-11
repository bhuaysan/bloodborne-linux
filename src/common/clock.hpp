// The runtime's process clock, shared by libkernel's counters and video_core's GPU clock.
#pragma once

#include <cstdint>

namespace bb::common {

// Nanoseconds of CLOCK_MONOTONIC since the process started (static initialisation, before the
// game runs).
std::uint64_t process_nsec();

}  // namespace bb::common
