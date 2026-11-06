// Direct memory (docs/design/m3-core-os.md, "Memory"): allocations of offset ranges in one sparse
// memfd, mapped through the region table of vmem.hpp. Internal to src/hle/kernel/.
#pragma once

#include <cstdint>

namespace bb::hle::kernel::dmem {

// What the total-size query returns, and the size of the memfd. The game uses it only as the end
// of every allocation's search range (eboot 0x33eacc, 0x2081a9b, 0x2081eaf, 0x26d3edd).
inline constexpr std::uint64_t total_size = std::uint64_t{8} << 30;

// Map flag 0x80, passed at eboot 0x29032a4. Its meaning is unknown; it is accepted and ignored.
inline constexpr int map_flag_0x80 = 0x80;

// The memfd, created at the first call. -1 (with errno set) if the host refused it.
int fd();

}  // namespace bb::hle::kernel::dmem

namespace bb::hle::kernel {

// The guest entry points; names unknown (docs/formats/kernel-imports.md, "Virtual and direct
// memory"). They return 0 or an sce error code.

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

}  // namespace bb::hle::kernel
