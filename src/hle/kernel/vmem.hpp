// Kernel virtual memory (docs/design/m3-core-os.md, "Memory"): the region table of guest
// mappings and the guest entry points. Internal to src/hle/kernel/; direct memory (#97) adds its
// mappings through map() with its own fd, flags and offsets.
#pragma once

#include <sys/types.h>

#include <cstdint>
#include <optional>
#include <string>

namespace bb::hle::kernel::vmem {

// The guest page size. Every guest mapping starts at a multiple of it and is a multiple of it
// long. The eboot rounds its map lengths to it (0x2081d8c) and passes it as an alignment
// (0x29032a9).
inline constexpr std::uint64_t page_size = 0x4000;

// The guest protection bits above PROT_EXEC. Accepted together with the others, recorded in the
// region table and dropped for the host. Logged once.
inline constexpr int prot_extra = 0x30;

// Region::flags, the flag byte that the eboot's mapping query reads (docs/formats/
// kernel-imports.md, rVjRvHJ0X6c).
inline constexpr std::uint32_t region_anonymous = 0x1;  // anonymous memory
inline constexpr std::uint32_t region_direct = 0x2;     // direct memory (#97)

// One guest mapping, or the part of one that a partial unmap or protect left.
struct Region {
    std::uintptr_t start = 0, end = 0;  // end is exclusive
    int prot = 0;                       // guest protection, prot_extra included
    std::uint32_t flags = 0;
    std::uint64_t offset = 0;           // direct memory offset of start, else 0
    std::string name;
};

// A mapping request for map().
struct Mapping {
    std::uintptr_t address = 0;  // a hint, or the exact address when `fixed`
    std::uint64_t length = 0;    // a non-zero multiple of page_size
    int prot = 0;                // guest protection
    int host_flags = 0;          // MAP_PRIVATE or MAP_SHARED, plus MAP_ANONYMOUS without fd
    bool fixed = false;          // never replaces a mapping (MAP_FIXED_NOREPLACE)
    int fd = -1;
    off_t fd_offset = 0;
    std::uint32_t flags = region_anonymous;  // Region::flags
    std::uint64_t offset = 0;                // Region::offset
    std::string name;
};

// Maps at a page_size-aligned host address and records the region. Returns 0 and sets `out`, or
// a host errno: EINVAL for a bad length, unaligned fixed address or unknown protection bits.
int map(const Mapping& mapping, std::uintptr_t& out);

// Unmaps [start, start + length rounded up to page_size). Returns 0 or a host errno: EINVAL if
// start is unaligned, length is 0, or part of the range is no guest mapping (so the runtime's
// own memory can never be unmapped).
int unmap(std::uintptr_t start, std::uint64_t length);

// Changes the protection of a range of guest mappings, with unmap()'s rules.
int protect(std::uintptr_t start, std::uint64_t length, int prot);

// The region that holds `address`.
std::optional<Region> find(std::uintptr_t address);

}  // namespace bb::hle::kernel::vmem

namespace bb::hle::kernel {

// The guest entry points. They return 0 or an sce error code (docs/design/m3-core-os.md,
// "Errors").

std::int32_t sceKernelMunmap(void* address, std::uint64_t length);
std::int32_t sceKernelMprotect(const void* address, std::uint64_t length, int prot);

}  // namespace bb::hle::kernel
