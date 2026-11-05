// Mutexes (docs/design/m3-core-os.md, "Sync"). The guest object is one 8-byte
// cell that holds a pointer to our object, or 0, the static initialiser. Values and call sites are
// in docs/formats/kernel-abi.md, "Mutexes and rwlocks". The entry points are registered by NID in
// sync.cpp; this header serves the other sync modules (condition variables).
#pragma once

#include <pthread.h>

#include <cstdint>

namespace bb::hle::kernel {

// Mutex types that the attribute's type setter takes (values of FreeBSD 10.4 include/pthread.h).
inline constexpr int guest_mutex_errorcheck = 1;
inline constexpr int guest_mutex_recursive = 2;
inline constexpr int guest_mutex_normal = 3;
inline constexpr int guest_mutex_adaptive = 4;

// The priority protocol of a new attribute (protocols are accepted, not applied).
inline constexpr int guest_prio_none = 0;

// The host mutex behind the guest mutex cell `mutex`. A cell that still holds the static
// initialiser gets an error-checking mutex, created under a global lock. Returns nullptr and sets
// `*host_errno` to EINVAL if `mutex` is null or its cell holds no mutex of ours.
pthread_mutex_t* host_mutex(std::uintptr_t* mutex, int* host_errno);

}  // namespace bb::hle::kernel
