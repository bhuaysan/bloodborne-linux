// Translation between the guest kernel ABI (FreeBSD 10, amd64) and the Linux host. Every value,
// with its source, is in docs/formats/kernel-abi.md. HLE functions use only these tables.
#pragma once

#include <cstdint>
#include <ctime>
#include <optional>

namespace bb::hle::kernel {

// ---- errno -------------------------------------------------------------------------------------

// The highest guest errno number.
inline constexpr int guest_elast = 96;

// The guest (FreeBSD) errno for a host errno, or std::nullopt if the guest has no such error.
std::optional<int> guest_errno(int host_errno);

// The host errno for a guest errno, or std::nullopt if the host has no such error or the number
// is not a guest errno.
std::optional<int> host_errno(int guest_errno);

// The sce error code of a guest errno: 0x80020000 | errno.
constexpr std::int32_t sce_error(int guest_errno) {
    return static_cast<std::int32_t>(0x80020000u | static_cast<std::uint32_t>(guest_errno));
}

// The guest errno that libc.prx derives from a negative sce result: the low 16 bits of a kernel
// code (bits 16-27 equal to 0x002), EIO for any other code.
constexpr int errno_from_sce(std::int32_t result) {
    const auto code = static_cast<std::uint32_t>(result);
    return (code & 0x0fff0000u) == 0x00020000u ? static_cast<int>(code & 0xffffu) : 5;
}

// The calling thread's guest errno: the int whose address the errno-location import hands to
// libc.prx and the eboot. Never glibc's errno. Zero in a new thread.
int* guest_errno_location();

// Error results by family (docs/design/m3-core-os.md, "Errors"), each from a host errno. A host
// errno without a guest counterpart becomes EIO and is reported on stderr once per value.

// open, read, mmap, clock_gettime, ...: sets the guest errno and returns -1.
int posix_fail(int host_errno);

// pthread_*: returns the guest errno number (0 for 0) and leaves the guest errno alone.
int pthread_error(int host_errno);

// sce*: returns the sce error code (0 for 0) and leaves the guest errno alone.
std::int32_t sce_fail(int host_errno);

// ---- flags -------------------------------------------------------------------------------------

// Translated flag bits, plus the input bits that have no counterpart. A caller that gets
// unknown bits logs them and refuses the call, unless it knows they are harmless.
struct Flags {
    int value = 0;
    int unknown = 0;
    bool operator==(const Flags&) const = default;
};

// open(2) flags. The access mode (low two bits) is the same on both sides; 3 is unknown.
Flags host_open_flags(int guest);
Flags guest_open_flags(int host);

// mmap(2) and mprotect(2) protection.
Flags host_prot(int guest);
Flags guest_prot(int host);

// mmap(2) flags.
Flags host_map_flags(int guest);
Flags guest_map_flags(int host);

// lseek(2) whence, or std::nullopt if it is not one.
std::optional<int> host_whence(int guest);

// The host clock for a guest clock id, or std::nullopt if the id is unassigned or has no host
// clock.
std::optional<clockid_t> host_clock(std::int32_t guest);

}  // namespace bb::hle::kernel
