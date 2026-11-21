// The counted sync object (counted_sync.cpp): what tests may ask about it. The entry points are
// registered by NID in counted_sync.cpp.
#pragma once

#include <cstddef>
#include <cstdint>

namespace bb::hle::kernel {

// The number of requests queued on the counted sync object `handle`, 0 for an unknown handle. For
// tests, which wait on it instead of sleeping until a thread has queued its request.
std::size_t counted_sync_waiters(std::uint64_t handle);

}  // namespace bb::hle::kernel
