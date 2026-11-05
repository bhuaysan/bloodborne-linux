// Differential tests of two small eboot functions against C++ versions written for these tests
// only. Real replacements live in bbgame.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>

#include <gtest/gtest.h>

#include "harness/eboot.hpp"
#include "harness/harness.hpp"

namespace {

using bb::harness::Arena;
using bb::harness::Result;
using bb::test::EbootTest;

void expect_report(const Result& r, std::string_view text) {
    EXPECT_FALSE(r);
    EXPECT_NE(r.report().find(text), std::string::npos) << r.report();
}

// An array of `count` items that the function at 0x101710 indexes.
struct Table {
    std::byte unknown[0x40];
    const std::uint64_t* items;
    std::uint64_t count;
};
static_assert(offsetof(Table, items) == 0x40 && offsetof(Table, count) == 0x48);

// 0x101710
std::uint64_t item_or_zero(const Table* t, std::uint32_t i) {
    return i < t->count ? t->items[i] : 0;
}

std::uint64_t item_or_zero_inclusive(const Table* t, std::uint32_t i) {
    return i <= t->count ? t->items[i] : 0;
}

std::uint64_t item_plus_one(const Table* t, std::uint32_t i) {
    return i < t->count ? t->items[i] + 1 : 0;
}

// A table of three items 0x100, 0x101, 0x102 in the arena, followed by a fourth that only an
// out-of-range read sees, called with index `i`.
auto table_at(std::uint32_t i) {
    return [i](Arena& a) {
        auto* items = a.zeroed<std::uint64_t>(4);
        for (std::uint64_t k = 0; k < 4; ++k) {
            items[k] = 0x100 + k;
        }
        auto* t = a.zeroed<Table>();
        t->items = items;
        t->count = 3;
        return std::tuple<const Table*, std::uint32_t>{t, i};
    };
}

using ItemFn = std::uint64_t(const Table*, std::uint32_t);

TEST_F(EbootTest, ItemOrZeroMatches) {
    EXPECT_EQ(image().function_size(0x101710), std::optional<std::uint64_t>{19});
    for (std::uint32_t i : {0U, 2U, 3U, 4U, 0xffffffffU}) {
        const Result r = harness().compare<ItemFn>(0x101710, &item_or_zero, table_at(i));
        EXPECT_TRUE(r) << "index " << i << ": " << r.report();
    }
}

TEST_F(EbootTest, ItemOrZeroCatchesWrongVersions) {
    expect_report(harness().compare<ItemFn>(0x101710, &item_or_zero_inclusive, table_at(3)),
                  "return value: original 0x0, replacement 0x103");
    expect_report(harness().compare<ItemFn>(0x101710, &item_plus_one, table_at(1)),
                  "return value: original 0x101, replacement 0x102");
}

// The output of the function at 0x10f890.
struct Pair {
    std::uintptr_t first, second;
    std::uint32_t tag;
};
static_assert(offsetof(Pair, tag) == 0x10);

// 0x10f890
void split(std::byte* a, Pair* out) {
    out->first = reinterpret_cast<std::uintptr_t>(a + 0x20);
    out->second = reinterpret_cast<std::uintptr_t>(a + 0x60);
    std::memcpy(&out->tag, a + 0x10, sizeof out->tag);
}

void split_wrong_second(std::byte* a, Pair* out) {
    split(a, out);
    out->second += 8;
}

// A 0x80-byte object at arena offset 0 with 0x11223344 at +0x10, and the output after it.
std::tuple<std::byte*, Pair*> object_and_pair(Arena& a) {
    auto* object = static_cast<std::byte*>(a.alloc(0x80));
    const std::uint32_t tag = 0x11223344;
    std::memcpy(object + 0x10, &tag, sizeof tag);
    return {object, a.zeroed<Pair>()};
}

TEST_F(EbootTest, SplitMatches) {
    EXPECT_EQ(image().function_size(0x10f890), std::optional<std::uint64_t>{22});
    const Result r = harness().compare<void(std::byte*, Pair*)>(0x10f890, &split, object_and_pair);
    EXPECT_TRUE(r) << r.report();
}

TEST_F(EbootTest, SplitCatchesAWrongWrite) {
    // The Pair starts at +0x80, so its second member at +0x88; the low byte differs by 8.
    expect_report(
        harness().compare<void(std::byte*, Pair*)>(0x10f890, &split_wrong_second, object_and_pair),
        "memory: arena +0x88..+0x89 differs: original 60, replacement 68");
}

}  // namespace
