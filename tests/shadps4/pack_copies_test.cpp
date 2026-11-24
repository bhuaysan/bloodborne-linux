// The copy shader HLE's staging layout (PackCopies in
// src/shadps4/video_core/renderer_vulkan/vk_shader_hle.h, #832): gathering the source runs into
// staging and copying the regions from it writes the same bytes as the copies in order, with
// sources and destinations that follow each other merged. Header-only, needs no Vulkan device.

#include <algorithm>
#include <cstdint>
#include <random>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "video_core/renderer_vulkan/vk_shader_hle.h"

namespace {

using Vulkan::CopyRange;
using Sources = std::vector<std::pair<std::uint64_t, std::uint64_t>>;

struct Packed {
    Sources sources;
    std::vector<CopyRange> regions;
    std::uint64_t total;
};

Packed Pack(const std::vector<CopyRange>& copies) {
    Packed packed;
    packed.total = Vulkan::PackCopies(copies, packed.sources, packed.regions);
    return packed;
}

TEST(PackCopies, Empty) {
    const Packed packed = Pack({});
    EXPECT_EQ(packed.total, 0u);
    EXPECT_TRUE(packed.sources.empty());
    EXPECT_TRUE(packed.regions.empty());
}

TEST(PackCopies, MergesSourcesAndDestinationsApart) {
    // Sources 0x100.. follow each other for the first two, destinations for the last two.
    const Packed packed = Pack({{0x100, 0x900, 0x10}, {0x110, 0x400, 0x20}, {0x500, 0x420, 0x8}});
    EXPECT_EQ(packed.total, 0x38u);
    EXPECT_EQ(packed.sources, (Sources{{0x100, 0x30}, {0x500, 0x8}}));
    EXPECT_EQ(packed.regions, (std::vector<CopyRange>{{0x0, 0x900, 0x10}, {0x10, 0x400, 0x28}}));
}

TEST(PackCopies, SkipsEmptyCopies) {
    const Packed packed = Pack({{0x10, 0x20, 0}, {0x40, 0x80, 4}, {0x0, 0x84, 0}, {0x44, 0x84, 4}});
    EXPECT_EQ(packed.total, 8u);
    EXPECT_EQ(packed.sources, (Sources{{0x40, 8}}));
    EXPECT_EQ(packed.regions, (std::vector<CopyRange>{{0, 0x80, 8}}));
}

// Random copies with disjoint destinations apart from the sources: staged and direct give the
// same destination bytes.
TEST(PackCopies, MatchesDirectCopies) {
    std::mt19937 rng{832};
    constexpr std::uint64_t kSlots = 64;
    constexpr std::uint64_t kStride = 12;
    for (int round = 0; round < 500; ++round) {
        std::vector<std::uint8_t> src(kSlots * kStride * 2);
        for (auto& b : src) {
            b = static_cast<std::uint8_t>(rng());
        }
        // Destination slots: a random order of disjoint runs; sources anywhere, often in order.
        std::vector<CopyRange> copies;
        std::uint64_t dst_slot = std::uniform_int_distribution<std::uint64_t>{0, 3}(rng);
        std::uint64_t src_slot = 0;
        while (dst_slot < kSlots) {
            const std::uint64_t n = std::uniform_int_distribution<std::uint64_t>{1, 3}(rng);
            if (dst_slot + n > kSlots) {
                break;
            }
            if (rng() % 3 == 0) {
                src_slot = std::uniform_int_distribution<std::uint64_t>{0, kSlots}(rng);
            }
            copies.push_back({src_slot * kStride, dst_slot * kStride, n * kStride});
            src_slot += n;
            dst_slot += n + (rng() % 2);
        }
        std::shuffle(copies.begin(), copies.end() - copies.size() / 2, rng);

        std::vector<std::uint8_t> direct(kSlots * kStride, 0);
        for (const auto& c : copies) {
            std::copy_n(src.begin() + c.src, c.size, direct.begin() + c.dst);
        }

        const Packed packed = Pack(copies);
        std::vector<std::uint8_t> staging;
        for (const auto& [offset, size] : packed.sources) {
            staging.insert(staging.end(), src.begin() + offset, src.begin() + offset + size);
        }
        ASSERT_EQ(staging.size(), packed.total);
        std::vector<std::uint8_t> staged(kSlots * kStride, 0);
        for (const auto& r : packed.regions) {
            ASSERT_LE(r.src + r.size, packed.total);
            std::copy_n(staging.begin() + r.src, r.size, staged.begin() + r.dst);
        }
        EXPECT_EQ(staged, direct);
        EXPECT_LE(packed.regions.size(), copies.size());
        EXPECT_LE(packed.sources.size(), copies.size());
    }
}

}  // namespace
