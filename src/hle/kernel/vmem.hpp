// Kernel virtual memory (docs/design/m3-core-os.md, "Memory"): the region table of guest
// mappings and the guest entry points. Internal to src/hle/kernel/; direct memory (#97) adds its
// mappings through map() with its own fd, flags, offsets and alignment, and maps into the
// reservations of reserve() (dmem.hpp).
// Exceptions: mapped() is also the GNM submits' check (src/hle/video/gnm_driver.cpp, PM4 readable),
// and DirectObserver is how the GPU memory mirror follows direct memory.
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

// The guest window [window_start, window_end): every mapping without a fixed address goes there,
// so that it fits the game's 40-bit GPU addresses. Reserved once, before the first guest mapping;
// an unmapped range in it is reserved again instead of returned to the host, and a range the host
// refuses to reserve again is never placed into again. It lies in
// AddressSanitizer's shadow gap, so ASan builds that map guest memory run with
// protect_shadow_gap=0, and above the gap's own shadow, which ASan then maps up to 0x4091ff7000.
inline constexpr std::uintptr_t window_start = 0x80'0000'0000;
inline constexpr std::uintptr_t window_end = 0x100'0000'0000;

// Whether [start, end) overlaps the guest window.
constexpr bool in_window(std::uintptr_t start, std::uintptr_t end) {
    return start < window_end && end > window_start;
}

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
    bool reserved = false;  // a reservation from reserve(): PROT_NONE, flags 0
    bool operator==(const Region&) const = default;
};

// A mapping request for map().
struct Mapping {
    std::uintptr_t address = 0;  // a hint, or the exact address when `fixed`
    std::uint64_t length = 0;    // a non-zero multiple of page_size
    int prot = 0;                // guest protection
    int host_flags = 0;          // MAP_PRIVATE or MAP_SHARED, plus MAP_ANONYMOUS without fd
    bool fixed = false;          // never replaces a mapping (MAP_FIXED_NOREPLACE), unless:
    bool into_reservation = false;  // with `fixed`: replaces the reservations that must cover
                                    // the range, and nothing else
    int fd = -1;
    off_t fd_offset = 0;
    std::uint32_t flags = region_anonymous;  // Region::flags
    std::uint64_t offset = 0;                // Region::offset
    std::uint64_t alignment = page_size;     // of the address the runtime picks: a power of two
    // A power of two or 0. The address the runtime picks in the window is ≡ `offset` (mod
    // congruent) where `alignment` allows it. A mapping at an address that isn't (the alignment
    // rules it out, or a fixed map) is logged.
    std::uint64_t congruent = 0;
    std::string name;
};

// Follows the views of direct memory for the GPU memory mirror (docs/design/graphics.md, "GPU
// memory mirror"). Called under the region table's lock, so that no other map or unmap of the
// range comes between the host's change and the call; it must not call into vmem.
class DirectObserver {
public:
    virtual ~DirectObserver() = default;
    // A direct region [address, address + length) that views direct memory at `offset` was
    // mapped; once per map.
    virtual void mapped(std::uintptr_t address, std::uint64_t length, std::uint64_t offset) = 0;
    // The part [address, address + length) of a direct region that views `offset` is about to be
    // unmapped or reserved again.
    virtual void unmapped(std::uintptr_t address, std::uint64_t length, std::uint64_t offset) = 0;
    // Direct memory [offset, offset + length) is about to be released (dmem.hpp), possibly still
    // mapped. Called under the allocation table's lock.
    virtual void released(std::uint64_t offset, std::uint64_t length) = 0;
};

// Sets the observer (null for none), before the first guest mapping. It must outlive the run.
void set_direct_observer(DirectObserver* observer);
// The observer, or null.
DirectObserver* direct_observer();

// Maps at the lowest free `alignment`-aligned address of the guest window, at or above the hint
// if it lies in the window (host MAP_32BIT: where the host picks), or at the fixed address, and
// records the region. A fixed map into the window takes a range that no region holds. Returns 0
// and sets `out`, or a host errno: EINVAL for a bad length or alignment, unaligned fixed address
// or unknown protection bits, also when a map into_reservation finds anything but reservations
// in its range; ENOMEM when the window is full; EEXIST when a fixed map hits a mapping or
// reaches across an end of the window.
int map(const Mapping& mapping, std::uintptr_t& out);

// Reserves `length` bytes of address space at an `alignment`-aligned address in the guest window
// (`hint` is a hint, as for map()): a PROT_NONE region with `reserved` set, which unmap() releases and only a map
// into_reservation replaces. Returns 0 and sets `out`, or a host errno as map().
int reserve(std::uintptr_t hint, std::uint64_t length, std::uint64_t alignment,
            std::uintptr_t& out);

// Turns [start, start + length rounded up to page_size) back into a reservation, with unmap()'s
// rules, so that a later unmap of the whole reservation finds no hole. If the host refuses, the
// range is no guest mapping any more.
int restore_reservation(std::uintptr_t start, std::uint64_t length);

// Unmaps [start, start + length rounded up to page_size); the part in the guest window is
// reserved again, so no host mapping can take it. Returns 0 or a host errno: EINVAL if
// start is unaligned, length is 0, or part of the range is no guest mapping (so the runtime's
// own memory can never be unmapped).
int unmap(std::uintptr_t start, std::uint64_t length);

// Changes the protection of a range of guest mappings, with unmap()'s rules. EINVAL if the range
// holds a reservation.
int protect(std::uintptr_t start, std::uint64_t length, int prot);

// Whether guest mappings cover [start, end) without a hole. False for an empty range.
bool mapped(std::uintptr_t start, std::uintptr_t end);

// The region that holds `address`.
std::optional<Region> find(std::uintptr_t address);

// A copy of the region table, ordered by address.
std::vector<Region> regions();

// For the differential test harness only (docs/design/runtime.md, "Guest memory"): puts the
// region table back to `baseline`, an earlier regions(). Gives back every region that isn't in
// it, as unmap() does, so the next map without a fixed address gets the same address again.
// Returns 0, EINVAL (and changes nothing) if a region of the baseline is gone or changed, which
// it can't map back, or the errno of a failed release.
int restore(const std::vector<Region>& baseline);

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

// Names in symbols/nids.csv (docs/formats/kernel-imports.md, "Virtual and direct memory").

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

// WFcfL2lzido: the bounds of the mapping that holds `address` (each output may be null) and its
// guest protection.
std::int32_t mapping_protection(const void* address, std::uint64_t* start, std::uint64_t* end,
                                std::uint32_t* prot);

}  // namespace bb::hle::kernel
