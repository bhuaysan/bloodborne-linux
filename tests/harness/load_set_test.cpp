// Differential tests of an eboot function that allocates, against C++ versions written for these
// tests only, with the load set loaded and initialised (LoadSetTest).
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

#include "game/image.hpp"
#include "harness/harness.hpp"
#include "harness/load_set.hpp"
#include "hle/hle.hpp"
#include "hle/kernel/abi.hpp"
#include "hle/kernel/dmem.hpp"
#include "hle/kernel/vmem.hpp"

namespace {

using bb::harness::Arena;
using bb::harness::Result;
using bb::test::LoadSetTest;
namespace dmem = bb::hle::kernel::dmem;
namespace vmem = bb::hle::kernel::vmem;

void expect_report(const Result& r, std::string_view text) {
    EXPECT_FALSE(r);
    EXPECT_NE(r.report().find(text), std::string::npos) << r.report();
}

void* guest_malloc(std::size_t size) {
    return reinterpret_cast<void* (*)(std::size_t)>(
        bb::hle::import_address("libc", "gQX+4GDQjpM"))(size);
}

// The size of the block that 0x3cc720 allocates for `kind`, 0 for none.
std::size_t block_size(int kind) {
    return kind == 2 ? 0x138 : kind == 1 ? 0x458 : 0;
}

// 0x3cc720: a zeroed block of 0x138 bytes for kind 2 and 0x458 for kind 1, null for other kinds
// and when malloc fails.
void* zeroed_block(int kind) {
    const std::size_t size = block_size(kind);
    if (size == 0) {
        return nullptr;
    }
    void* p = guest_malloc(size);
    if (p != nullptr) {
        std::memset(p, 0, size);
    }
    return p;
}

void* zeroed_block_too_large(int kind) {
    const std::size_t size = block_size(kind);
    void* p = size == 0 ? nullptr : guest_malloc(size + 0x10);
    if (p != nullptr) {
        std::memset(p, 0, size);
    }
    return p;
}

void* zeroed_block_setting_errno(int kind) {
    *bb::hle::kernel::guest_errno_location() = 12;
    return zeroed_block(kind);
}

auto kind(int k) {
    return [k](Arena&) { return std::tuple<int>{k}; };
}

using BlockFn = void*(int);

TEST_F(LoadSetTest, ZeroedBlockMatches) {
    EXPECT_EQ(image().function_size(0x3cc720), std::optional<std::uint64_t>{0x46});
    for (int k : {0, 1, 2, 3, -1}) {
        const Result r = harness().compare<BlockFn>(0x3cc720, &zeroed_block, kind(k));
        EXPECT_TRUE(r) << "kind " << k << ": " << r.report();
    }
}

// The block comes from the game's heap, which the baseline already holds: the call maps nothing.
TEST_F(LoadSetTest, BlocksComeFromTheHeapInTheBaseline) {
    const std::size_t mappings = bb::hle::kernel::vmem::regions().size();
    void* p = bb::game::fn<BlockFn>(0x3cc720)(1);
    ASSERT_NE(p, nullptr);
    const auto region = bb::hle::kernel::vmem::find(reinterpret_cast<std::uintptr_t>(p));
    ASSERT_TRUE(region.has_value());
    EXPECT_NE(region->flags & bb::hle::kernel::vmem::region_direct, 0U);
    EXPECT_EQ(bb::hle::kernel::vmem::regions().size(), mappings);
    // The restore takes the allocation back: the same call gets the same block again.
    harness().restore();
    EXPECT_EQ(bb::game::fn<BlockFn>(0x3cc720)(1), p);
}

TEST_F(LoadSetTest, ZeroedBlockCatchesWrongVersions) {
    expect_report(harness().compare<BlockFn>(0x3cc720, &zeroed_block_too_large, kind(2)),
                  "memory: mapping ");
    expect_report(harness().compare<BlockFn>(0x3cc720, &zeroed_block_setting_errno, kind(1)),
                  "memory: guest errno +0x0..+0x1 differs: original 00, replacement 0c");
}

// Each compare starts from the baseline again, so repeated ones agree. Records the time per
// compare, which the sparse heap keeps in milliseconds.
TEST_F(LoadSetTest, RepeatedComparesStartFromTheBaseline) {
    constexpr int compares = 10;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < compares; ++i) {
        const Result r = harness().compare<BlockFn>(0x3cc720, &zeroed_block, kind(2));
        EXPECT_TRUE(r) << r.report();
    }
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start);
    RecordProperty("us_per_compare", static_cast<int>(us.count() / compares));
}

// Map calls (docs/design/runtime.md, "Guest memory"). No eboot function that maps is a small
// leaf: the map calls at 0x2081dbe and 0x33eb0d sit in allocator methods that lock and fill their
// own tables. So the originals here are synthetic guest-ABI functions that call the imports as
// those call sites do.

template <class F>
F* kernel_import(std::string_view nid) {
    return reinterpret_cast<F*>(bb::hle::import_address("libkernel", nid));
}

// The anonymous map, IWIBBdTHit4: (&address, length, prot, flags).
std::int32_t map_anonymous(void** address, std::uint64_t length, int prot) {
    return kernel_import<std::int32_t(void**, std::uint64_t, int, int)>("IWIBBdTHit4")(
        address, length, prot, 0);
}

// Maps `length` rounded up to the guest page, read and write, as 0x2081d8c to 0x2081dbe does.
void* map_pages(std::uint64_t length) {
    void* at = nullptr;
    return map_anonymous(&at, (length + 0x3fff) & ~std::uint64_t{0x3fff}, 3) == 0 ? at : nullptr;
}

void* map_pages_again(std::uint64_t length) {
    void* at = nullptr;
    const std::uint64_t rounded = length % 0x4000 == 0 ? length : length - length % 0x4000 + 0x4000;
    if (map_anonymous(&at, rounded, 3) != 0) {
        return nullptr;
    }
    return at;
}

void* map_pages_rounded_to_32k(std::uint64_t length) {
    void* at = nullptr;
    return map_anonymous(&at, (length + 0x7fff) & ~std::uint64_t{0x7fff}, 3) == 0 ? at : nullptr;
}

void* map_pages_read_only(std::uint64_t length) {
    void* at = nullptr;
    return map_anonymous(&at, (length + 0x3fff) & ~std::uint64_t{0x3fff}, 1) == 0 ? at : nullptr;
}

// Allocates `allocated` bytes of direct memory aligned to 64 KiB (rTXw65xmLIA) and maps the first
// `length` of them (L-Q3LEjIbgA), as 0x26d3ee0 and 0x26d3fda do. Null on failure.
void* map_direct_with(std::uint64_t allocated, std::uint64_t length) {
    const std::uint64_t total = kernel_import<std::uint64_t()>("pO96TwzOm5E")();
    std::uint64_t offset = 0;
    if (kernel_import<std::int32_t(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                   std::int32_t, std::uint64_t*)>("rTXw65xmLIA")(
            0, total, allocated, 0x10000, 0, &offset) != 0) {
        return nullptr;
    }
    void* at = nullptr;
    return kernel_import<std::int32_t(void**, std::uint64_t, int, int, std::uint64_t,
                                      std::uint64_t)>("L-Q3LEjIbgA")(&at, length, 0x33, 0,
                                                                     offset, 0x10000) == 0
               ? at
               : nullptr;
}

void* map_direct_pages(std::uint64_t length) {
    return map_direct_with(length, length);
}

void* map_direct_pages_allocating_twice(std::uint64_t length) {
    return map_direct_with(2 * length, length);
}

using MapFn = void*(std::uint64_t);

auto length(std::uint64_t n) {
    return [n](Arena&) { return std::tuple<std::uint64_t>{n}; };
}

TEST_F(LoadSetTest, MapCallsMatch) {
    for (std::uint64_t n : {0x4000, 0x5000, 0x10000}) {
        const Result r = harness().compare_functions<MapFn>(&map_pages, &map_pages_again,
                                                            length(n));
        EXPECT_TRUE(r) << std::hex << n << ": " << r.report();
    }
    const Result r =
        harness().compare_functions<MapFn>(&map_direct_pages, &map_direct_pages, length(0x10000));
    EXPECT_TRUE(r) << r.report();
}

TEST_F(LoadSetTest, MapCallsCatchWrongVersions) {
    // Placement is deterministic, so this is where both calls map.
    const auto at = reinterpret_cast<std::uintptr_t>(map_pages(0x4000));
    harness().restore();
    const auto mapping = [&](std::uint64_t size, int prot) {
        return std::format("mapping {:#x}..{:#x} prot {:#x} flags 0x1 offset 0x0\n", at, at + size,
                           prot);
    };
    // The same address, but not the same size, protection or allocation.
    const Result larger =
        harness().compare_functions<MapFn>(&map_pages, &map_pages_rounded_to_32k, length(0x4000));
    expect_report(larger, "guest memory: differs at change 1 (original 1 changes, replacement 1)\n"
                          "  original 1: " +
                              mapping(0x4000, 3) + "  replacement 1: " + mapping(0x8000, 3));
    expect_report(
        harness().compare_functions<MapFn>(&map_pages, &map_pages_read_only, length(0x4000)),
        "  replacement 1: " + mapping(0x4000, 1));
    expect_report(harness().compare_functions<MapFn>(&map_direct_pages,
                                                     &map_direct_pages_allocating_twice,
                                                     length(0x10000)),
                  "guest memory: differs at change 2 (original 2 changes, replacement 2)");
}

std::optional<vmem::Region> heap;  // a read-write mapping of the baseline

int protects_the_heap(std::uint64_t prot) {
    return vmem::protect(heap->start, heap->end - heap->start, static_cast<int>(prot));
}

// A replacement that changes a baseline mapping fails without reading the regions, which it may
// have made unreadable. The original protects the mapping as it already is.
TEST_F(LoadSetTest, MapCallsCatchAChangedBaselineMapping) {
    const std::vector<vmem::Region> mappings = vmem::regions();
    const auto it = std::ranges::find(mappings, 3, &vmem::Region::prot);
    ASSERT_NE(it, mappings.end());
    heap = *it;
    const Result r = harness().compare_functions<int(std::uint64_t)>(
        &protects_the_heap, +[](std::uint64_t) { return protects_the_heap(0); }, length(3));
    const auto mapping = [&](int prot) {
        return std::format("mapping {:#x}..{:#x} prot {:#x} flags {:#x} offset {:#x}", heap->start,
                           heap->end, prot, heap->flags, heap->offset);
    };
    expect_report(r, "memory: not compared, the guest memory baseline changed\nguest memory: "
                     "differs at change 1 (original 0 changes, replacement 2)\n  replacement 1: " +
                         mapping(3) + " removed\n  replacement 2: " + mapping(0) + "\n");
    ASSERT_EQ(protects_the_heap(3), 0);  // the baseline back for the next test
}

// The restore takes back the mappings and the allocations, so the same calls get the same
// addresses and offsets again, and read zeros.
TEST_F(LoadSetTest, RepeatedMapsGetTheSameAddresses) {
    const std::vector<vmem::Region> mappings = vmem::regions();
    const std::vector<dmem::AllocatedRange> allocations = dmem::allocations();
    auto* pages = static_cast<char*>(map_pages(0x4000));
    auto* direct = static_cast<char*>(map_direct_pages(0x10000));
    ASSERT_NE(pages, nullptr);
    ASSERT_NE(direct, nullptr);
    EXPECT_TRUE(vmem::in_window(reinterpret_cast<std::uintptr_t>(pages),
                                reinterpret_cast<std::uintptr_t>(pages) + 1));
    EXPECT_EQ(dmem::allocations().size(), allocations.size() + 1);
    pages[0] = 1;
    direct[0] = 2;
    harness().restore();
    EXPECT_EQ(vmem::regions(), mappings);
    EXPECT_EQ(dmem::allocations(), allocations);
    EXPECT_FALSE(vmem::find(reinterpret_cast<std::uintptr_t>(pages)).has_value());
    EXPECT_EQ(map_pages(0x4000), pages);
    EXPECT_EQ(map_direct_pages(0x10000), direct);
    EXPECT_EQ(pages[0], 0);
    EXPECT_EQ(direct[0], 0);
}

}  // namespace
