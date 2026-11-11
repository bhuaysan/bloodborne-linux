#include "hle/kernel/dmem.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "common/log.hpp"
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

// udmabuf imports only from a memfd sealed against shrinking (the GPU memory mirror).
TEST(KernelDmem, MemfdIsSealedAgainstShrinking) {
    ASSERT_GE(dmem::fd(), 0);
    EXPECT_NE(::fcntl(dmem::fd(), F_GET_SEALS) & F_SEAL_SHRINK, 0);
}

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
    for (void* const p : {a, b}) {  // where the game's 40-bit GPU addresses reach them
        EXPECT_TRUE(addr(p) >= vmem::window_start && addr(p) + 2 * page_size <= vmem::window_end)
            << p;
    }
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

// Mappings keep window offset ≡ direct memory offset (mod mirror_block), which the GPU memory
// mirror binds by, also for a page in the middle of a block and when a hint asks elsewhere.
TEST(KernelDmem, MappingsAreCongruentToTheirOffsetModTheMirrorBlock) {
    const std::uint64_t offset = allocate(4 * page_size, dmem::mirror_block);
    std::vector<void*> views;
    for (std::uint64_t k = 0; k < 4; ++k) {
        void* p = reinterpret_cast<void*>(vmem::window_start + 0x10'0000'0000 + page_size);
        ASSERT_EQ(map_direct(&p, page_size, 3, 0, offset + k * page_size, 0), 0);
        EXPECT_EQ((addr(p) - vmem::window_start) % dmem::mirror_block, k * page_size) << k;
        views.push_back(p);
    }
    // An alignment above the page keeps it where it can: 32 KiB-aligned at offset 2 pages.
    void* p = nullptr;
    ASSERT_EQ(map_direct(&p, page_size, 3, 0, offset + 2 * page_size, 2 * page_size), 0);
    EXPECT_EQ(addr(p) % dmem::mirror_block, 2 * page_size);
    views.push_back(p);
    for (void* const v : views) {
        EXPECT_EQ(sceKernelMunmap(v, page_size), 0);
    }
    EXPECT_EQ(release_direct(offset, 4 * page_size), 0);
}

// A view the mirror can't bind exactly is logged: an alignment whose multiples the offset is not
// one of (32 KiB at an odd page, 64 KiB at two pages), or a batch map that the game places at an
// address not congruent to its offset. Congruent views are not logged.
TEST(KernelDmem, MapsThatBreakTheCongruenceAreLogged) {
    const std::uint64_t offset = allocate(4 * page_size, dmem::mirror_block);
    void* range = nullptr;
    ASSERT_EQ(reserve_range(&range, 2 * page_size, 0, dmem::mirror_block), 0);
    const std::uintptr_t r = addr(range);
    const BatchEntry entries[2] = {
        {r, offset + page_size, page_size, 3, {}, dmem::batch_map},
        {r + page_size, offset + page_size, page_size, 3, {}, dmem::batch_map},
    };
    const struct {
        std::uint64_t page, alignment;  // of a map_direct; alignment 0 or 1: that batch entry
        bool logged;
    } cases[] = {{1, 2 * page_size, true}, {2, 4 * page_size, true}, {2, 2 * page_size, false},
                 {3, page_size, false},    {0, 0, true},             {0, 1, false}};
    bb::common::set_log_filter(bb::common::LogFilter{});  // info and above
    std::vector<void*> views;
    for (const auto& c : cases) {
        testing::internal::CaptureStderr();
        if (c.alignment > 1) {
            views.push_back(map(offset + c.page * page_size, page_size, c.alignment));
        } else {
            EXPECT_EQ(map_direct_batch(&entries[c.alignment], 1, nullptr), 0);
        }
        const std::string log = testing::internal::GetCapturedStderr();
        EXPECT_EQ(log.find("not congruent mod 0x10000") != std::string::npos, c.logged)
            << c.page << " " << c.alignment << ": " << log;
    }
    for (void* const v : views) {
        EXPECT_EQ(sceKernelMunmap(v, page_size), 0);
    }
    EXPECT_EQ(sceKernelMunmap(range, 2 * page_size), 0);
    EXPECT_EQ(release_direct(offset, 4 * page_size), 0);
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

TEST(KernelDmem, TypeQueryReportsTheAllocation) {
    const std::uint64_t first = allocate(2 * page_size, 0, 3);
    const std::uint64_t second = allocate(page_size, 0, 0);
    ASSERT_EQ(second, first + 2 * page_size);  // adjacent, and kept apart
    std::int32_t type = -1;
    std::uint64_t start = 0, end = 0;
    ASSERT_EQ(direct_memory_type(first + page_size + 0x10, &type, &start, &end), 0);
    EXPECT_EQ(type, 3);
    EXPECT_EQ(start, first);
    EXPECT_EQ(end, first + 2 * page_size);
    ASSERT_EQ(direct_memory_type(second, &type, &start, &end), 0);
    EXPECT_EQ(type, 0);
    EXPECT_EQ(start, second);
    EXPECT_EQ(end, second + page_size);
    // A partial release splits the bounds.
    EXPECT_EQ(release_direct(first, page_size), 0);
    ASSERT_EQ(direct_memory_type(first + page_size, &type, &start, &end), 0);
    EXPECT_EQ(type, 3);
    EXPECT_EQ(start, first + page_size);
    // Free offsets and null outputs fail and change nothing.
    type = 7;
    EXPECT_NE(direct_memory_type(first, &type, &start, &end), 0);
    EXPECT_NE(direct_memory_type(second + page_size, &type, &start, &end), 0);
    EXPECT_EQ(type, 7);
    EXPECT_EQ(direct_memory_type(second, nullptr, &start, &end), sce_einval);
    EXPECT_EQ(direct_memory_type(second, &type, &start, nullptr), sce_einval);

    EXPECT_EQ(release_direct(first + page_size, page_size), 0);
    EXPECT_EQ(release_direct(second, page_size), 0);
}

// The eboot's type-3 test (0x26bae30): the mapping query's offset, then the type query.
TEST(KernelDmem, TypeThreeTestOfTheEbootAnswersTrue) {
    const std::uint64_t offset = allocate(0x10000, 0x10000, 3);  // as 0x26d3ee0
    auto* const p = static_cast<char*>(map(offset, 0x10000, 0x10000));
    QueryRecord record{};
    ASSERT_EQ(query_mapping(p + page_size, 0, &record, sizeof record), 0);
    std::int32_t type = -1;
    std::uint64_t start = 0, end = 0;
    ASSERT_EQ(direct_memory_type(record.offset, &type, &start, &end), 0);
    EXPECT_EQ(type, 3);
    EXPECT_EQ(sceKernelMunmap(p, 0x10000), 0);
    EXPECT_EQ(release_direct(offset, 0x10000), 0);
}

TEST(KernelDmem, ReserveRefusesBadArguments) {
    void* p = nullptr;
    EXPECT_EQ(reserve_range(&p, page_size, 1, 0), sce_einval);         // flags
    EXPECT_EQ(reserve_range(&p, page_size, 0, 0x6000), sce_einval);    // alignment
    EXPECT_EQ(reserve_range(&p, 0x1000, 0, 0), sce_einval);            // length
    EXPECT_EQ(reserve_range(&p, 0, dmem::map_flag_0x80, 0), sce_einval);
    EXPECT_EQ(reserve_range(nullptr, page_size, 0, 0), sce_einval);
    EXPECT_EQ(p, nullptr);
}

// As 0x2901eb0: reserve a range, map blocks of direct memory into it, and undo as 0x2902101.
TEST(KernelDmem, BatchMapsIntoAReservationAndUndoes) {
    const std::uint64_t offset = allocate(2 * page_size, 0x10000);
    auto* const view = static_cast<char*>(map(offset, 2 * page_size, 0x10000));
    std::memcpy(view + page_size, "block", 6);
    void* range = nullptr;
    ASSERT_EQ(reserve_range(&range, 4 * page_size, dmem::map_flag_0x80, 0), 0);
    auto* const base = static_cast<char*>(range);
    const auto reserved = vmem::find(addr(base));
    ASSERT_TRUE(reserved);
    EXPECT_TRUE(reserved->reserved);
    EXPECT_EQ(reserved->end, addr(base) + 4 * page_size);

    BatchEntry entries[2] = {
        {addr(base), offset + page_size, page_size, 3, {}, dmem::batch_map},
        {addr(base) + 2 * page_size, offset, page_size, 0x33, {}, dmem::batch_map},
    };
    std::int32_t done = -1;
    ASSERT_EQ(map_direct_batch(entries, 2, &done), 0);
    EXPECT_EQ(done, 2);
    EXPECT_STREQ(base, "block");  // aliases the view
    QueryRecord record{};
    ASSERT_EQ(query_mapping(base + 2 * page_size + 0x10, 0, &record, sizeof record), 0);
    EXPECT_EQ(record.start, addr(base) + 2 * page_size);
    EXPECT_EQ(record.offset, offset);
    EXPECT_EQ(record.flags, vmem::region_direct);
    EXPECT_EQ(vmem::find(addr(base) + 2 * page_size)->prot, 0x33);
    EXPECT_TRUE(vmem::find(addr(base) + page_size)->reserved);  // the gap stays reserved

    for (BatchEntry& e : entries) {
        e.operation = dmem::batch_unmap;
    }
    ASSERT_EQ(map_direct_batch(entries, 2, nullptr), 0);
    EXPECT_TRUE(vmem::find(addr(base))->reserved);
    EXPECT_TRUE(vmem::find(addr(base) + 2 * page_size)->reserved);
    EXPECT_EQ(sceKernelMunmap(base, 4 * page_size), 0);  // 0x2902115: no hole left
    EXPECT_FALSE(vmem::find(addr(base)));
    EXPECT_STREQ(view + page_size, "block");

    EXPECT_EQ(sceKernelMunmap(view, 2 * page_size), 0);
    EXPECT_EQ(release_direct(offset, 2 * page_size), 0);
}

// Keeps an observer registered for its scope, so that a test that returns early never leaves one
// behind.
struct Registered {
    vmem::MappingObserver* observer;
    explicit Registered(vmem::MappingObserver* o) : observer(o) {
        added = vmem::add_mapping_observer(o);
    }
    ~Registered() {
        vmem::remove_mapping_observer(observer);
    }
    Registered(const Registered&) = delete;
    Registered& operator=(const Registered&) = delete;
    bool added = false;
};

// Records the calls of a mapping observer as {kind, address, length, offset}.
struct Recorder : vmem::MappingObserver {
    struct Call {
        char kind;  // 'm'apped, 'u'nmapped, 'r'eleased (address 0), 'p'rotection changed
        std::uint64_t address, length, offset;
        bool operator==(const Call&) const = default;
    };
    std::vector<Call> calls;
    void mapped(std::uintptr_t a, std::uint64_t l, std::uint64_t o) override {
        calls.push_back({'m', a, l, o});
    }
    void unmapped(std::uintptr_t a, std::uint64_t l, std::uint64_t o) override {
        calls.push_back({'u', a, l, o});
    }
    void released(std::uint64_t o, std::uint64_t l) override { calls.push_back({'r', 0, l, o}); }
    void protection_changed(std::uintptr_t a, std::uint64_t l) override {
        calls.push_back({'p', a, l, 0});
    }
};

// A guest mprotect tells the observers of the whole range it changed, page-rounded, so that the
// write tracking drops the pages whose protection the guest replaced; one that fails tells
// nothing.
TEST(KernelDmem, TheObserverHearsOfProtectionChanges) {
    Recorder recorder;
    const Registered registered(&recorder);
    ASSERT_TRUE(registered.added);
    const std::uint64_t offset = allocate(2 * page_size, 0);
    void* const direct = map(offset, 2 * page_size);
    const std::uintptr_t a = addr(direct);
    void* range = nullptr;
    ASSERT_EQ(reserve_range(&range, page_size, 0, 0), 0);
    EXPECT_EQ(sceKernelMprotect(reinterpret_cast<void*>(a + page_size), 0x10, 1), 0);
    EXPECT_EQ(vmem::find(a + page_size)->prot, 1);
    EXPECT_EQ(sceKernelMprotect(direct, 2 * page_size, 3), 0);
    EXPECT_EQ(sceKernelMprotect(direct, page_size, 0x100), sce_einval);  // bad protection
    EXPECT_EQ(sceKernelMprotect(range, page_size, 3), sce_einval);       // reserved
    EXPECT_EQ(sceKernelMunmap(direct, 2 * page_size), 0);
    EXPECT_EQ(sceKernelMunmap(range, page_size), 0);
    EXPECT_EQ(release_direct(offset, 2 * page_size), 0);
    vmem::remove_mapping_observer(&recorder);
    const std::vector<Recorder::Call> expected{
        {'m', a, 2 * page_size, offset},
        {'p', a + page_size, page_size, 0},
        {'p', a, 2 * page_size, 0},
        {'u', a, page_size, offset},
        {'u', a + page_size, page_size, offset + page_size},
        {'r', 0, 2 * page_size, offset},
    };
    EXPECT_EQ(recorder.calls, expected);
}

// The observer sees every map and unmap of direct and flexible memory, in parts with their own
// offsets, and releases, also of a range still mapped; reservations it doesn't see.
TEST(KernelDmem, TheObserverFollowsDirectAndFlexibleMemory) {
    Recorder recorder;
    const Registered registered(&recorder);
    ASSERT_TRUE(registered.added);
    const std::uint64_t offset = allocate(3 * page_size, 0);
    const std::uintptr_t a = addr(map(offset, 3 * page_size));
    void* anonymous = nullptr;
    ASSERT_EQ(map_anonymous(&anonymous, page_size, 3, 0), 0);
    const std::uint64_t flexible = vmem::find(addr(anonymous))->backing;
    EXPECT_GE(flexible, dmem::total_size);
    void* range = nullptr;
    ASSERT_EQ(reserve_range(&range, 2 * page_size, 0, 0), 0);
    const std::uintptr_t r = addr(range);
    BatchEntry entry{r + page_size, offset + 2 * page_size, page_size, 3, {}, dmem::batch_map};
    ASSERT_EQ(map_direct_batch(&entry, 1, nullptr), 0);
    // Unmapping the middle page splits the view; the whole range then goes in two parts.
    EXPECT_EQ(sceKernelMunmap(reinterpret_cast<void*>(a + page_size), page_size), 0);
    EXPECT_EQ(sceKernelMunmap(reinterpret_cast<void*>(a), 3 * page_size), sce_einval);
    EXPECT_EQ(sceKernelMunmap(reinterpret_cast<void*>(a), page_size), 0);
    EXPECT_EQ(release_direct(offset, 2 * page_size), 0);
    // The last page goes while two views of it are still mapped (0x26d3f70); they unmap later.
    EXPECT_EQ(release_direct(offset + 2 * page_size, page_size), 0);
    entry.operation = dmem::batch_unmap;
    ASSERT_EQ(map_direct_batch(&entry, 1, nullptr), 0);
    EXPECT_EQ(sceKernelMunmap(reinterpret_cast<void*>(a + 2 * page_size), page_size), 0);
    EXPECT_EQ(sceKernelMunmap(range, 2 * page_size), 0);
    EXPECT_EQ(sceKernelMunmap(anonymous, page_size), 0);
    vmem::remove_mapping_observer(&recorder);
    const std::vector<Recorder::Call> expected{
        {'m', a, 3 * page_size, offset},
        {'m', addr(anonymous), page_size, flexible},
        {'m', r + page_size, page_size, offset + 2 * page_size},
        {'u', a + page_size, page_size, offset + page_size},
        {'u', a, page_size, offset},
        {'r', 0, 2 * page_size, offset},
        {'r', 0, page_size, offset + 2 * page_size},
        {'u', r + page_size, page_size, offset + 2 * page_size},
        {'u', a + 2 * page_size, page_size, offset + 2 * page_size},
        {'u', addr(anonymous), page_size, flexible},
    };
    EXPECT_EQ(recorder.calls, expected);
}

TEST(KernelDmem, BatchStopsAtTheFirstFailure) {
    const std::uint64_t offset = allocate(2 * page_size, 0);
    void* range = nullptr;
    ASSERT_EQ(reserve_range(&range, 2 * page_size, 0, 0), 0);
    const std::uintptr_t base = addr(range);
    void* other = nullptr;
    ASSERT_EQ(map_anonymous(&other, page_size, 3, 0), 0);
    std::memset(other, 0x5a, page_size);

    // The second entry maps over a mapping that isn't reserved: refused, the first stays done.
    const BatchEntry entries[2] = {
        {base, offset, page_size, 3, {}, dmem::batch_map},
        {addr(other), offset, page_size, 3, {}, dmem::batch_map},
    };
    std::int32_t done = -1;
    EXPECT_NE(map_direct_batch(entries, 2, &done), 0);
    EXPECT_EQ(done, 1);
    EXPECT_EQ(static_cast<const unsigned char*>(other)[0], 0x5a);
    EXPECT_EQ(vmem::find(base)->flags, vmem::region_direct);

    // The checks of the batch itself (vmem's tests cover the address range): a free offset and an
    // unknown operation.
    const BatchEntry bad[2] = {
        {base + page_size, offset + 2 * page_size, page_size, 3, {}, dmem::batch_map},
        {base + page_size, offset, page_size, 3, {}, 2},
    };
    EXPECT_NE(map_direct_batch(&bad[0], 1, &done), 0);
    EXPECT_NE(map_direct_batch(&bad[1], 1, &done), 0);
    EXPECT_TRUE(vmem::find(base + page_size)->reserved);
    EXPECT_EQ(map_direct_batch(nullptr, 1, &done), sce_einval);
    EXPECT_EQ(map_direct_batch(entries, -1, &done), sce_einval);
    EXPECT_EQ(map_direct_batch(entries, 0, &done), 0);
    EXPECT_EQ(done, 0);

    const BatchEntry undo{base, 0, page_size, 0, {}, dmem::batch_unmap};
    EXPECT_EQ(map_direct_batch(&undo, 1, nullptr), 0);
    EXPECT_EQ(sceKernelMunmap(range, 2 * page_size), 0);
    EXPECT_EQ(sceKernelMunmap(other, page_size), 0);
    EXPECT_EQ(release_direct(offset, 2 * page_size), 0);
}

// The harness's restore (docs/design/runtime.md, "Guest memory"): takes back what was mapped
// and allocated after the baseline, also a split of a baseline allocation, and refuses a
// baseline whose mappings changed. The data of the baseline's allocations is the harness's to
// restore, so only the new allocation's must read zeros.
TEST(KernelDmem, HarnessRestoreTakesBackMappingsAndAllocations) {
    const std::uint64_t kept = allocate(2 * page_size, 0);
    void* kept_view = map(kept, 2 * page_size);
    const std::vector<vmem::Region> mappings = vmem::regions();
    const std::vector<dmem::AllocatedRange> allocations = dmem::allocations();

    const std::uint64_t added = allocate(2 * page_size, 0);
    EXPECT_EQ(release_direct(kept + page_size, page_size), 0);
    auto* view = static_cast<unsigned char*>(map(added, 2 * page_size));
    std::memset(view, 0xa5, 2 * page_size);
    EXPECT_EQ(sceKernelMunmap(view + page_size, page_size), 0);  // split
    void* anonymous = nullptr;
    EXPECT_EQ(map_anonymous(&anonymous, page_size, 3, 0), 0);
    EXPECT_EQ(vmem::restore(mappings), 0);
    EXPECT_EQ(dmem::restore_allocations(allocations), 0);
    EXPECT_EQ(vmem::regions(), mappings);
    EXPECT_EQ(dmem::allocations(), allocations);

    // The same calls get the same offset and address again, and read zeros.
    EXPECT_EQ(allocate(2 * page_size, 0), added);
    EXPECT_EQ(map(added, 2 * page_size), view);
    EXPECT_EQ(view[0], 0);
    EXPECT_EQ(view[page_size], 0);
    EXPECT_EQ(vmem::restore(mappings), 0);
    EXPECT_EQ(dmem::restore_allocations(allocations), 0);

    // A baseline mapping that is gone can't be mapped back: nothing changes.
    EXPECT_EQ(sceKernelMunmap(kept_view, page_size), 0);
    const std::vector<vmem::Region> now = vmem::regions();
    EXPECT_EQ(vmem::restore(mappings), EINVAL);
    EXPECT_EQ(vmem::regions(), now);
    EXPECT_EQ(sceKernelMunmap(static_cast<char*>(kept_view) + page_size, page_size), 0);
    EXPECT_EQ(release_direct(kept, 2 * page_size), 0);
}

// Flexible memory lives in the memfd above direct memory: what the guest writes is in the memfd
// at the region's backing and the reverse. Unmapping gives the backing back with a hole punched,
// so the next map reads zeros again. The anonymous mmap is flexible memory as well.
TEST(KernelDmem, FlexibleMemoryLivesInTheMemfd) {
    const std::uint64_t free_before = dmem::flexible_free();
    void* p = nullptr;
    ASSERT_EQ(map_anonymous(&p, 3 * page_size, 3, 0), 0);
    const std::optional<vmem::Region> region = vmem::find(addr(p));
    ASSERT_TRUE(region);
    EXPECT_EQ(region->offset, 0u);  // what the mapping query reports
    const std::uint64_t backing = region->backing;
    EXPECT_GE(backing, total_size);
    EXPECT_LE(backing + 3 * page_size, dmem::memfd_size);
    EXPECT_EQ(dmem::flexible_free(), free_before - 3 * page_size);

    auto* const bytes = static_cast<char*>(p);
    std::strcpy(bytes + page_size + 8, "guest");
    char read[6] = {};
    ASSERT_EQ(::pread(dmem::fd(), read, sizeof read, static_cast<off_t>(backing + page_size + 8)),
              6);
    EXPECT_STREQ(read, "guest");
    ASSERT_EQ(::pwrite(dmem::fd(), "memfd", 6, static_cast<off_t>(backing + 2 * page_size)), 6);
    EXPECT_STREQ(bytes + 2 * page_size, "memfd");

    // A partial unmap splits the backing with the region and gives back only its page.
    ASSERT_EQ(sceKernelMunmap(bytes + page_size, page_size), 0);
    EXPECT_EQ(vmem::find(addr(bytes) + 2 * page_size)->backing, backing + 2 * page_size);
    EXPECT_EQ(dmem::flexible_free(), free_before - 2 * page_size);
    ASSERT_EQ(::pread(dmem::fd(), read, sizeof read, static_cast<off_t>(backing + page_size + 8)),
              6);
    EXPECT_EQ(read[0], 0);
    ASSERT_EQ(sceKernelMunmap(bytes, page_size), 0);
    ASSERT_EQ(sceKernelMunmap(bytes + 2 * page_size, page_size), 0);
    EXPECT_EQ(dmem::flexible_free(), free_before);

    void* again = guest_mmap(nullptr, 3 * page_size, 3, 0x1000 | 0x2, -1, 0);  // MAP_ANON, PRIVATE
    ASSERT_NE(again, reinterpret_cast<void*>(-1));
    EXPECT_EQ(vmem::find(addr(again))->backing, backing);
    EXPECT_EQ(static_cast<char*>(again)[2 * page_size], 0);
    EXPECT_EQ(dmem::flexible_free(), free_before - 3 * page_size);
    EXPECT_EQ(guest_munmap(again, 3 * page_size), 0);
    EXPECT_EQ(dmem::flexible_free(), free_before);
}

// More flexible memory than the area holds is refused, and the area stays as it was.
TEST(KernelDmem, FlexibleMemoryEndsWithItsArea) {
    const std::uint64_t free_before = dmem::flexible_free();
    void* p = nullptr;
    EXPECT_EQ(map_anonymous(&p, dmem::flexible_size + page_size, 3, 0), sce_enomem);
    EXPECT_EQ(dmem::flexible_free(), free_before);
}

// An observer that looks up the region table from another thread, as the GPU thread does while
// the observer waits for it. It records whether the lookup finished, which it can't while the
// kernel holds the table's lock, and whether it found the region still there: an unmap notice
// comes before the region and its host mapping go. The lookups are kept until the probe goes, so
// that a lookup still waiting for the lock never blocks the observer.
struct LockProbe : vmem::MappingObserver {
    std::vector<bool> finished;
    std::vector<bool> present;
    std::vector<std::future<std::optional<vmem::Region>>> lookups;
    void probe(std::uintptr_t address) {
        lookups.push_back(
            std::async(std::launch::async, [address] { return vmem::find(address); }));
        const bool ready =
            lookups.back().wait_for(std::chrono::seconds(5)) == std::future_status::ready;
        finished.push_back(ready);
        if (ready) {
            const std::optional<vmem::Region> region = lookups.back().get();
            present.push_back(region && !region->reserved && region->backing != vmem::no_backing);
        }
    }
    void mapped(std::uintptr_t a, std::uint64_t, std::uint64_t) override { probe(a); }
    void unmapped(std::uintptr_t a, std::uint64_t, std::uint64_t) override { probe(a); }
    void released(std::uint64_t, std::uint64_t) override {}
};

TEST(KernelDmem, TheObserverRunsWithoutTheRegionLock) {
    LockProbe probe;
    std::optional<Registered> registered(&probe);
    ASSERT_TRUE(registered->added);
    const std::uint64_t offset = allocate(page_size, 0);
    void* const direct = map(offset, page_size);
    void* flexible = nullptr;
    EXPECT_EQ(map_anonymous(&flexible, page_size, 3, 0), 0);
    void* range = nullptr;
    ASSERT_EQ(reserve_range(&range, page_size, 0, 0), 0);
    BatchEntry entry{addr(range), offset, page_size, 3, {}, dmem::batch_map};
    EXPECT_EQ(map_direct_batch(&entry, 1, nullptr), 0);
    entry.operation = dmem::batch_unmap;  // reserved again
    EXPECT_EQ(map_direct_batch(&entry, 1, nullptr), 0);
    EXPECT_EQ(sceKernelMunmap(direct, page_size), 0);
    EXPECT_EQ(sceKernelMunmap(flexible, page_size), 0);
    EXPECT_EQ(sceKernelMunmap(range, page_size), 0);
    EXPECT_EQ(release_direct(offset, page_size), 0);
    registered.reset();
    EXPECT_EQ(probe.finished, std::vector<bool>(6, true));
    EXPECT_EQ(probe.present, std::vector<bool>(6, true));
}

// Observers are kept side by side up to the limit, and a removed one sees nothing more.
TEST(KernelDmem, ObserversAreAddedAndRemoved) {
    struct Counter : vmem::MappingObserver {
        int maps = 0;
        void mapped(std::uintptr_t, std::uint64_t, std::uint64_t) override { ++maps; }
        void unmapped(std::uintptr_t, std::uint64_t, std::uint64_t) override {}
        void released(std::uint64_t, std::uint64_t) override {}
    };
    std::vector<Counter> counters(vmem::max_mapping_observers + 1);
    std::vector<std::unique_ptr<Registered>> registered;
    for (std::size_t i = 0; i < vmem::max_mapping_observers; ++i) {
        registered.push_back(std::make_unique<Registered>(&counters[i]));
        ASSERT_TRUE(registered.back()->added);
    }
    registered.push_back(std::make_unique<Registered>(&counters.back()));
    EXPECT_FALSE(registered.back()->added);
    vmem::remove_mapping_observer(&counters[1]);
    void* p = nullptr;
    EXPECT_EQ(map_anonymous(&p, page_size, 3, 0), 0);
    EXPECT_EQ(sceKernelMunmap(p, page_size), 0);
    for (std::size_t i = 0; i < counters.size(); ++i) {
        vmem::remove_mapping_observer(&counters[i]);
        EXPECT_EQ(counters[i].maps, i == 1 || i == vmem::max_mapping_observers ? 0 : 1) << i;
    }
}

TEST(KernelDmem, ImportsResolveToTheImplementations) {
    const auto resolve = [](const char* nid) {
        return bb::hle::resolve({nid, "libkernel", "libkernel", bb::loader::stt_func});
    };
    EXPECT_EQ(resolve("pO96TwzOm5E"), addr(reinterpret_cast<void*>(&direct_memory_size)));
    EXPECT_EQ(resolve("rTXw65xmLIA"), addr(reinterpret_cast<void*>(&allocate_direct)));
    EXPECT_EQ(resolve("L-Q3LEjIbgA"), addr(reinterpret_cast<void*>(&map_direct)));
    EXPECT_EQ(resolve("MBuItvba6z8"), addr(reinterpret_cast<void*>(&release_direct)));
    EXPECT_EQ(resolve("BC+OG5m9+bw"), addr(reinterpret_cast<void*>(&direct_memory_type)));
    EXPECT_EQ(resolve("7oxv3PPCumo"), addr(reinterpret_cast<void*>(&reserve_range)));
    EXPECT_EQ(resolve("2SKEx6bSq-4"), addr(reinterpret_cast<void*>(&map_direct_batch)));
}

}  // namespace
