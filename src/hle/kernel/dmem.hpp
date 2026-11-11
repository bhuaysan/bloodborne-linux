// Direct memory (docs/design/m3-core-os.md, "Memory"): allocations of offset ranges in one sparse
// memfd, mapped through the region table of vmem.hpp. Internal to src/hle/kernel/.
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace bb::hle::kernel::dmem {

// What the total-size query returns, and the size of the memfd. The game uses it only as the end
// of every allocation's search range (eboot 0x33eacc, 0x2081a9b, 0x2081eaf, 0x26d3edd).
inline constexpr std::uint64_t total_size = std::uint64_t{8} << 30;

// Flexible memory (vmem::Mapping::flexible) lives in the same memfd, above direct memory, so that
// one backing holds every page the GPU can see (docs/design/renderer.md, "One backing for every
// page the GPU can see"). The memfd is sparse, so the size costs only the pages the game touches.
inline constexpr std::uint64_t flexible_size = std::uint64_t{8} << 30;
// The size of the memfd: direct memory at [0, total_size), flexible memory above it.
inline constexpr std::uint64_t memfd_size = total_size + flexible_size;

// Map flag 0x80, passed at eboot 0x29032a4, and the reservation's flag 0x80 (0x2901fbd). Its
// meaning is unknown; it is accepted and ignored.
inline constexpr int map_flag_0x80 = 0x80;

// The binding block of the GPU memory mirror (docs/design/graphics.md, "GPU memory mirror"): the
// largest sparse buffer granularity of the drivers checked (RADV). map_direct places a mapping at
// an address ≡ its offset (mod mirror_block), so that the mirror can bind the memfd's blocks.
inline constexpr std::uint64_t mirror_block = 0x10000;

// The operations of a BatchEntry.
inline constexpr std::int32_t batch_map = 0;    // map the offset range into a reservation
inline constexpr std::int32_t batch_unmap = 1;  // unmap the range back into a reservation

// The memfd, created at the first call. -1 (with errno set) if the host refused it.
int fd();

// Takes `length` bytes, a non-zero multiple of the page size, at the lowest free offset of the
// flexible area. They read zeros. Nothing when no such range is free or there is no memfd.
std::optional<std::uint64_t> allocate_flexible(std::uint64_t length);

// Gives [offset, offset + length) of the flexible area back, which allocate_flexible() took, and
// punches a hole there, so that it reads zeros when it is taken again. No guest mapping may view
// it any more.
void free_flexible(std::uint64_t offset, std::uint64_t length);

// The free bytes of the flexible area, for tests.
std::uint64_t flexible_free();

// One allocation, as allocations() lists it.
struct AllocatedRange {
    std::uint64_t start = 0, end = 0;  // offsets in the memfd, end exclusive
    std::int32_t type = 0;
    bool operator==(const AllocatedRange&) const = default;
};

// For the differential test harness only (docs/design/runtime.md, "Guest memory"): a copy of
// the allocations, ordered by offset.
std::vector<AllocatedRange> allocations();

// For the harness only: puts the allocations back to `baseline`, an earlier allocations(), and
// punches a hole into the memfd wherever an allocation lies now that the baseline doesn't
// cover, so the next allocation there gets the same offset and reads zeros again. The data of
// the baseline's allocations is the harness's to restore. Returns 0 or the errno of fallocate.
int restore_allocations(const std::vector<AllocatedRange>& baseline);

}  // namespace bb::hle::kernel::dmem

namespace bb::hle::kernel {

// The guest entry points (docs/formats/kernel-imports.md, "Virtual and direct memory"), named in
// symbols/nids.csv. They return 0 or an sce error code.

// pO96TwzOm5E: dmem::total_size.
std::uint64_t direct_memory_size();

// rTXw65xmLIA: allocates `length` bytes at the lowest `alignment`-aligned offset in
// [search_start, search_end) that is free, records `type` with them and stores the offset.
std::int32_t allocate_direct(std::uint64_t search_start, std::uint64_t search_end,
                             std::uint64_t length, std::uint64_t alignment, std::int32_t type,
                             std::uint64_t* offset);

// L-Q3LEjIbgA: maps allocated direct memory at an `alignment`-aligned address. *address is a
// hint in and the mapping out.
std::int32_t map_direct(void** address, std::uint64_t length, int prot, int flags,
                        std::uint64_t offset, std::uint64_t alignment);

// MBuItvba6z8: releases allocated direct memory; the length is rounded up to the page size.
std::int32_t release_direct(std::uint64_t offset, std::uint64_t length);

// BC+OG5m9+bw: the type and the bounds [*start, *end) of the allocation that holds `offset`.
std::int32_t direct_memory_type(std::uint64_t offset, std::int32_t* type, std::uint64_t* start,
                                std::uint64_t* end);

// 7oxv3PPCumo: reserves `length` bytes of address space at an `alignment`-aligned address for
// map_direct_batch(). *address is a hint in and the range out.
std::int32_t reserve_range(void** address, std::uint64_t length, int flags,
                           std::uint64_t alignment);

// One operation of map_direct_batch(), as 0x2901eb0 fills it.
struct BatchEntry {
    std::uint64_t address;  // inside a reservation
    std::uint64_t offset;   // direct memory offset, for batch_map
    std::uint64_t length;
    std::uint8_t prot;      // guest protection, for batch_map
    std::uint8_t reserved[3];
    std::int32_t operation;  // dmem::batch_map or dmem::batch_unmap
};
static_assert(sizeof(BatchEntry) == 0x20);

// 2SKEx6bSq-4: runs `count` entries in order and stops at the first that fails. *done (if not
// null) is the number of entries that succeeded.
std::int32_t map_direct_batch(const BatchEntry* entries, std::int32_t count, std::int32_t* done);

}  // namespace bb::hle::kernel
