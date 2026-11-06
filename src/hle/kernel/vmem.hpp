// Kernel virtual memory (docs/design/m3-core-os.md, "Memory"): the region table of guest
// mappings and the guest entry points. Internal to src/hle/kernel/; direct memory (#97) adds its
// mappings through map() with its own fd, flags, offsets and alignment (dmem.hpp).
#pragma once

#include <sys/types.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bb::hle::kernel::vmem {

// The guest page size. Every guest mapping starts at a multiple of it and is a multiple of it
// long. The eboot rounds its map lengths to it (0x2081d8c) and passes it as an alignment
// (0x29032a9).
inline constexpr std::uint64_t page_size = 0x4000;

// The guest protection bits above PROT_EXEC. Accepted together with the others, recorded in the
// region table and dropped for the host. Logged once.
inline constexpr int prot_extra = 0x30;

// Region::flags, the byte at +0x20 of the query record. The eboot picks the release call by them
// (0x20826e1).
inline constexpr std::uint32_t region_anonymous = 0x1;  // released with release_anonymous
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
    std::uint64_t alignment = page_size;     // of the address the host picks: a power of two
    std::string name;
};

// Maps at an `alignment`-aligned host address (or at the fixed one) and records the region.
// Returns 0 and sets `out`, or a host errno: EINVAL for a bad length or alignment, unaligned
// fixed address or unknown protection bits.
int map(const Mapping& mapping, std::uintptr_t& out);

// Unmaps [start, start + length rounded up to page_size). Returns 0 or a host errno: EINVAL if
// start is unaligned, length is 0, or part of the range is no guest mapping (so the runtime's
// own memory can never be unmapped).
int unmap(std::uintptr_t start, std::uint64_t length);

// Changes the protection of a range of guest mappings, with unmap()'s rules.
int protect(std::uintptr_t start, std::uint64_t length, int prot);

// Whether guest mappings cover [start, end) without a hole. False for an empty range.
bool mapped(std::uintptr_t start, std::uintptr_t end);

// The region that holds `address`.
std::optional<Region> find(std::uintptr_t address);

// A copy of the region table, ordered by address.
std::vector<Region> regions();

}  // namespace bb::hle::kernel::vmem

namespace bb::hle::kernel {

// The guest entry points. POSIX calls return -1 (MAP_FAILED for mmap) and set the guest errno,
// the others return 0 or an sce error code (docs/design/m3-core-os.md, "Errors").

void* guest_mmap(void* address, std::uint64_t length, int prot, int flags, int fd,
                 std::int64_t offset);
int guest_munmap(void* address, std::uint64_t length);
int guest_msync(void* address, std::uint64_t length, int flags);
int guest_getpagesize();

std::int32_t sceKernelMunmap(void* address, std::uint64_t length);
std::int32_t sceKernelMprotect(const void* address, std::uint64_t length, int prot);
std::int32_t sceKernelMlock(const void* address, std::uint64_t length);

// Names unknown (docs/formats/kernel-imports.md, "Virtual and direct memory").

// mL8NDH86iQI: maps anonymous memory. *address is a hint in and the mapping out.
std::int32_t map_named_anonymous(void** address, std::uint64_t length, int prot, int flags,
                                 const char* name);
// IWIBBdTHit4: the same without a name.
std::int32_t map_anonymous(void** address, std::uint64_t length, int prot, int flags);
// teiItL2boFw: releases memory from the anonymous map calls.
std::int32_t release_anonymous(void* address, std::uint64_t length);

// rVjRvHJ0X6c: fills a QueryRecord for the mapping that holds `address`.
std::int32_t query_mapping(const void* address, int flags, void* record, std::uint64_t size);

// The record of query_mapping. Only these fields are read by the game; the rest is zero.
struct QueryRecord {
    std::uint64_t start;   // read at 0x2902063
    std::uint64_t end;     // exclusive
    std::uint64_t offset;  // read at 0x26d3f67, 0x2902063, 0x2903295
    std::uint8_t reserved1[8];
    std::uint8_t flags;    // Region::flags, read at 0x20826de, 0x2902031, 0x2903174
    std::uint8_t reserved2[0x27];
};
static_assert(sizeof(QueryRecord) == 0x48);

// BC+OG5m9+bw: (direct memory offset, &type, &start, &end). Fails for every offset until direct
// memory's types are read from dmem.cpp (follow-up issue; m3-core-os.md, Memory, "Not yet").
std::int32_t direct_memory_type(std::uint64_t offset, std::int32_t* type, std::uint64_t* start,
                                std::uint64_t* end);

// WFcfL2lzido: the bounds of the mapping that holds `address` (each output may be null) and its
// guest protection.
std::int32_t mapping_protection(const void* address, std::uint64_t* start, std::uint64_t* end,
                                std::uint32_t* prot);

}  // namespace bb::hle::kernel
