#include "hle/kernel/dmem.hpp"

#include <cstdint>
#include <cstring>

#include <gtest/gtest.h>

#include "hle/hle.hpp"
#include "hle/kernel/vmem.hpp"

namespace {

using namespace bb::hle::kernel;
using dmem::total_size;
using vmem::page_size;

constexpr std::int32_t sce_enomem = static_cast<std::int32_t>(0x8002000cu);
constexpr std::int32_t sce_einval = static_cast<std::int32_t>(0x80020016u);

std::uintptr_t addr(const void* p) {
    return reinterpret_cast<std::uintptr_t>(p);
}

// Allocates as the eboot does: search the whole of direct memory (0x2081a9e).
std::uint64_t allocate(std::uint64_t length, std::uint64_t alignment, std::int32_t type = 0) {
    std::uint64_t offset = ~std::uint64_t{0};
    EXPECT_EQ(allocate_direct(0, direct_memory_size(), length, alignment, type, &offset), 0);
    return offset;
}

void* map(std::uint64_t offset, std::uint64_t length, std::uint64_t alignment = page_size,
          int prot = 3, int flags = 0) {
    void* p = nullptr;
    EXPECT_EQ(map_direct(&p, length, prot, flags, offset, alignment), 0);
    return p;
}

// Each test releases what it allocates, so every test starts with all of direct memory free.

TEST(KernelDmem, AllocationRespectsAlignmentAndRange) {
    const std::uint64_t first = allocate(page_size, 0);  // 0: the guest page
    EXPECT_EQ(first, 0u);
    // The lowest free offset with the alignment: past the first allocation.
    const std::uint64_t aligned = allocate(page_size, 0x10000);
    EXPECT_EQ(aligned, 0x10000u);
    // The allocation lies inside [start, end): here exactly at the start.
    std::uint64_t ranged = 0;
    ASSERT_EQ(allocate_direct(0x40000, 0x48000, 2 * page_size, page_size, 0, &ranged), 0);
    EXPECT_EQ(ranged, 0x40000u);
    // Alignments below the guest page are raised to it.
    std::uint64_t small = 0;
    ASSERT_EQ(allocate_direct(0x4000, total_size, page_size, 0x1000, 0, &small), 0);
    EXPECT_EQ(small, 0x4000u);
    // A range that the length doesn't fit in once aligned, and a full one.
    std::uint64_t none = 7;
    EXPECT_EQ(allocate_direct(0x81000, 0x88000, page_size, 0x8000, 0, &none), sce_enomem);
    EXPECT_EQ(allocate_direct(0x40000, 0x48000, page_size, page_size, 0, &none), sce_enomem);
    EXPECT_EQ(none, 7u);
    // Bad arguments.
    EXPECT_EQ(allocate_direct(0, total_size, page_size, 0x6000, 0, &none), sce_einval);
    EXPECT_EQ(allocate_direct(0, total_size, 0x1000, page_size, 0, &none), sce_einval);
    EXPECT_EQ(allocate_direct(0, total_size, 0, page_size, 0, &none), sce_einval);
    EXPECT_EQ(allocate_direct(0x8000, 0x8000, page_size, page_size, 0, &none), sce_einval);
    EXPECT_EQ(allocate_direct(0, total_size, page_size, page_size, 0, nullptr), sce_einval);

    EXPECT_EQ(release_direct(first, page_size), 0);
    EXPECT_EQ(release_direct(aligned, page_size), 0);
    EXPECT_EQ(release_direct(ranged, 2 * page_size), 0);
    EXPECT_EQ(release_direct(small, page_size), 0);
}

TEST(KernelDmem, ExhaustedMemoryGivesTheGuestError) {
    const std::uint64_t all = allocate(total_size, 0);
    EXPECT_EQ(all, 0u);
    std::uint64_t offset = 7;
    EXPECT_EQ(allocate_direct(0, total_size, page_size, 0, 0, &offset), sce_enomem);
    EXPECT_EQ(offset, 7u);
    EXPECT_EQ(release_direct(all, total_size), 0);
}

TEST(KernelDmem, TwoMappingsOfOneRangeAlias) {
    const std::uint64_t offset = allocate(2 * page_size, 0x10000);
    void* const a = map(offset, 2 * page_size, 0x10000);
    // The second mapping as 0x2903220 makes it: flag 0x80, the guest page as alignment.
    void* const b = map(offset, 2 * page_size, page_size, 3, dmem::map_flag_0x80);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_NE(a, b);
    EXPECT_EQ(addr(a) % 0x10000, 0u);
    std::memcpy(static_cast<char*>(a) + page_size, "alias", 6);
    EXPECT_STREQ(static_cast<const char*>(b) + page_size, "alias");

    // Both are direct regions with the offset, as the query reports it.
    QueryRecord record{};
    ASSERT_EQ(query_mapping(static_cast<char*>(b) + page_size, 0, &record, sizeof record), 0);
    EXPECT_EQ(record.start, addr(b));
    EXPECT_EQ(record.offset, offset);
    EXPECT_EQ(record.flags, vmem::region_direct);

    EXPECT_EQ(sceKernelMunmap(a, 2 * page_size), 0);
    EXPECT_EQ(sceKernelMunmap(b, 2 * page_size), 0);
    EXPECT_EQ(release_direct(offset, 2 * page_size), 0);
}

TEST(KernelDmem, MapRefusesBadArguments) {
    const std::uint64_t offset = allocate(page_size, 0);
    void* p = nullptr;
    EXPECT_EQ(map_direct(&p, page_size, 3, 1, offset, 0), sce_einval);              // flags
    EXPECT_EQ(map_direct(&p, page_size, 3, 0, offset, 0x6000), sce_einval);         // alignment
    EXPECT_EQ(map_direct(&p, 0x1000, 3, 0, offset, 0), sce_einval);                 // length
    EXPECT_EQ(map_direct(&p, 2 * page_size, 3, 0, offset, 0), sce_einval);          // past the end
    EXPECT_EQ(map_direct(&p, page_size, 3, 0, offset + page_size, 0), sce_einval);  // free
    EXPECT_EQ(map_direct(&p, page_size, 0x100, 0, offset, 0), sce_einval);          // protection
    EXPECT_EQ(map_direct(nullptr, page_size, 3, 0, offset, 0), sce_einval);
    EXPECT_EQ(p, nullptr);
    EXPECT_EQ(release_direct(offset, page_size), 0);
}

TEST(KernelDmem, ReleaseFreesTheRange) {
    const std::uint64_t offset = allocate(4 * page_size, 0, 3);
    // A partial release, with an unrounded length as at 0x33eb1d, splits the allocation.
    EXPECT_EQ(release_direct(offset + page_size, page_size - 0x100), 0);
    // The hole is free again, and nothing else is.
    EXPECT_EQ(allocate(page_size, 0), offset + page_size);
    EXPECT_EQ(allocate(page_size, 0), offset + 4 * page_size);
    EXPECT_EQ(release_direct(offset + 4 * page_size, page_size), 0);
    EXPECT_EQ(release_direct(offset + page_size, page_size), 0);
    // Releasing or mapping a range that isn't all allocated fails and changes nothing.
    void* p = nullptr;
    EXPECT_EQ(map_direct(&p, 2 * page_size, 3, 0, offset, 0), sce_einval);
    EXPECT_EQ(release_direct(offset, 2 * page_size), sce_einval);
    EXPECT_EQ(release_direct(offset + 0x100, page_size), sce_einval);
    EXPECT_EQ(release_direct(offset, 0), sce_einval);
    p = map(offset, page_size);
    EXPECT_EQ(sceKernelMunmap(p, page_size), 0);

    EXPECT_EQ(release_direct(offset, page_size), 0);
    EXPECT_EQ(release_direct(offset + 2 * page_size, 2 * page_size), 0);
    EXPECT_EQ(allocate(4 * page_size, 0), offset);
    EXPECT_EQ(release_direct(offset, 4 * page_size), 0);
}

TEST(KernelDmem, ReleasedMemoryReadsZerosWhenAllocatedAgain) {
    const std::uint64_t offset = allocate(page_size, 0);
    void* p = map(offset, page_size);
    std::memset(p, 0xa5, page_size);
    EXPECT_EQ(sceKernelMunmap(p, page_size), 0);
    EXPECT_EQ(release_direct(offset, page_size), 0);

    EXPECT_EQ(allocate(page_size, 0), offset);
    p = map(offset, page_size);
    EXPECT_EQ(static_cast<const unsigned char*>(p)[0], 0);
    EXPECT_EQ(sceKernelMunmap(p, page_size), 0);
    EXPECT_EQ(release_direct(offset, page_size), 0);
}

// The eboot's heap at start-up (0x2081a80): 116 MiB aligned to 2 MiB, mapped read/write.
TEST(KernelDmem, GameSizedAllocationSucceeds) {
    constexpr std::uint64_t length = 0x7400000, alignment = 0x200000;
    EXPECT_EQ(direct_memory_size(), total_size);
    const std::uint64_t offset = allocate(length, alignment);
    EXPECT_EQ(offset % alignment, 0u);
    auto* const p = static_cast<unsigned char*>(map(offset, length, alignment));
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(addr(p) % alignment, 0u);
    p[0] = 1;
    p[length - 1] = 2;
    EXPECT_EQ(p[length - 1], 2);
    const auto region = vmem::find(addr(p) + length - 1);
    ASSERT_TRUE(region);
    EXPECT_EQ(region->end - region->start, length);
    EXPECT_EQ(sceKernelMunmap(p, length), 0);
    EXPECT_EQ(release_direct(offset, length), 0);
}

TEST(KernelDmem, ImportsResolveToTheImplementations) {
    const auto resolve = [](const char* nid) {
        return bb::hle::resolve({nid, "libkernel", "libkernel", bb::loader::stt_func});
    };
    EXPECT_EQ(resolve("pO96TwzOm5E"), addr(reinterpret_cast<void*>(&direct_memory_size)));
    EXPECT_EQ(resolve("rTXw65xmLIA"), addr(reinterpret_cast<void*>(&allocate_direct)));
    EXPECT_EQ(resolve("L-Q3LEjIbgA"), addr(reinterpret_cast<void*>(&map_direct)));
    EXPECT_EQ(resolve("MBuItvba6z8"), addr(reinterpret_cast<void*>(&release_direct)));
}

}  // namespace
