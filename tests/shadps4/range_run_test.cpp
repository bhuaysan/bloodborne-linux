// The runs of the copy shader's destination ranges (ForEachRangeRun in
// src/shadps4/video_core/renderer_vulkan/vk_shader_hle.h, #676): the ranges it reports cover
// exactly the bytes of the input ranges in any order, and for ranges in ascending order they are
// the union, ascending, with no two overlapping or touching. Header-only, needs no Vulkan device.

#include <algorithm>
#include <cstdint>
#include <random>
#include <set>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "video_core/renderer_vulkan/vk_shader_hle.h"

namespace {

using Ranges = std::vector<std::pair<std::uint64_t, std::uint64_t>>;

Ranges Runs(const Ranges& ranges) {
    Ranges out;
    Vulkan::ForEachRangeRun(ranges, [&](std::uint64_t start, std::uint64_t size) {
        out.emplace_back(start, start + size);
    });
    return out;
}

std::set<std::uint64_t> Bytes(const Ranges& ranges) {
    std::set<std::uint64_t> bytes;
    for (const auto& [start, end] : ranges) {
        for (std::uint64_t b = start; b < end; ++b) {
            bytes.insert(b);
        }
    }
    return bytes;
}

TEST(RangeRun, Empty) {
    EXPECT_TRUE(Runs({}).empty());
}

TEST(RangeRun, MergesOverlappingAndTouching) {
    EXPECT_EQ(Runs({{0x10, 0x20}, {0x18, 0x1c}, {0x20, 0x28}, {0x30, 0x40}}),
              (Ranges{{0x10, 0x28}, {0x30, 0x40}}));
    EXPECT_EQ(Runs({{0x10, 0x20}, {0x10, 0x20}}), (Ranges{{0x10, 0x20}}));
    // A range below the run that reaches into it joins the run too.
    EXPECT_EQ(Runs({{0x20, 0x30}, {0x10, 0x20}, {0x08, 0x12}}), (Ranges{{0x08, 0x30}}));
}

TEST(RangeRun, KeepsGapsAndOrder) {
    EXPECT_EQ(Runs({{0x10, 0x20}, {0x21, 0x30}}), (Ranges{{0x10, 0x20}, {0x21, 0x30}}));
    EXPECT_EQ(Runs({{0x40, 0x50}, {0x10, 0x20}, {0x48, 0x60}}),
              (Ranges{{0x40, 0x50}, {0x10, 0x20}, {0x48, 0x60}}));
}

// Random ranges against a byte set: the same bytes in any order; sorted, the union.
TEST(RangeRun, MatchesByteSet) {
    std::mt19937 rng{676};
    for (int round = 0; round < 200; ++round) {
        Ranges ranges;
        const int n = std::uniform_int_distribution<int>{1, 30}(rng);
        for (int i = 0; i < n; ++i) {
            const std::uint64_t start = std::uniform_int_distribution<std::uint64_t>{0, 400}(rng);
            const std::uint64_t size = std::uniform_int_distribution<std::uint64_t>{1, 40}(rng);
            ranges.emplace_back(start, start + size);
        }
        EXPECT_EQ(Bytes(Runs(ranges)), Bytes(ranges));

        std::ranges::sort(ranges);
        const Ranges sorted = Runs(ranges);
        EXPECT_EQ(Bytes(sorted), Bytes(ranges));
        for (std::size_t i = 0; i < sorted.size(); ++i) {
            ASSERT_LT(sorted[i].first, sorted[i].second);
            if (i > 0) {
                ASSERT_LT(sorted[i - 1].second, sorted[i].first);
            }
        }
    }
}

}  // namespace
