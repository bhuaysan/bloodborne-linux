// Differential tests of an eboot function that allocates, against C++ versions written for these
// tests only, with the load set loaded and initialised (LoadSetTest).
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>

#include <gtest/gtest.h>

#include "game/image.hpp"
#include "harness/harness.hpp"
#include "harness/load_set.hpp"
#include "hle/hle.hpp"
#include "hle/kernel/abi.hpp"
#include "hle/kernel/vmem.hpp"

namespace {

using bb::harness::Arena;
using bb::harness::Result;
using bb::test::LoadSetTest;

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

}  // namespace
