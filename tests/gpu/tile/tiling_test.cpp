#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "gpu/tile/tiling.hpp"

namespace {

using namespace bb::tile;

constexpr std::uint32_t supported[] = {0, 1, 2, 3, 4, 5, 8, 9, 10, 13, 14, 31};

Layout make(const Surface& s) {
    const auto l = layout(s);
    EXPECT_TRUE(l.has_value());
    return l.value_or(Layout{});
}

TEST(Tiling, ModeTable) {
    for (std::uint32_t mode = 0; mode < 32; ++mode) {
        bool listed = false;
        for (const std::uint32_t m : supported) {
            listed = listed || m == mode;
        }
        EXPECT_EQ(mode_info(mode).has_value(), listed) << mode;
    }
}

TEST(Tiling, DisplayBufferLayout) {
    // The game's display buffers: 1920x1080, 32 bits, Display2DThin (docs/design/graphics.md).
    const Layout l = make({.tile_mode = 10, .bits = 32, .pitch = 1920, .height = 1080});
    EXPECT_EQ(l.banks, 16u);
    EXPECT_EQ(l.bank_height, 1u);
    EXPECT_EQ(l.macro_width, 128u);
    EXPECT_EQ(l.macro_height, 64u);
    ASSERT_EQ(l.levels.size(), 1u);
    EXPECT_EQ(l.levels[0].pitch, 1920u);
    EXPECT_EQ(l.levels[0].height, 1088u);
    EXPECT_EQ(l.size, 1920u * 1088 * 4);
}

TEST(Tiling, GameTextureSizes) {
    // Sizes of Thin1DThin textures as stored in the game's TPF files: BC1 1024x1024 with 11
    // levels, BC1 2048x2048 with 12, BC7 2048x2048 with 12. Levels below 8x8 blocks still take
    // a whole micro tile.
    EXPECT_EQ(make({.tile_mode = 13, .bits = 64, .pitch = 1024, .height = 1024, .levels = 11,
                    .blocks = true})
                  .size,
              0xab400u);
    EXPECT_EQ(make({.tile_mode = 13, .bits = 64, .pitch = 2048, .height = 2048, .levels = 12,
                    .blocks = true})
                  .size,
              0x2ab400u);
    EXPECT_EQ(make({.tile_mode = 13, .bits = 128, .pitch = 2048, .height = 2048, .levels = 12,
                    .blocks = true})
                  .size,
              0x556800u);
}

TEST(Tiling, PaddingRules) {
    // Linear aligned, 1 byte: pitch to 64, then until a layer is a multiple of 256 elements.
    EXPECT_EQ(make({.tile_mode = 8, .bits = 8, .pitch = 100, .height = 3}).levels[0].pitch, 256u);
    // Linear general keeps the pitch.
    EXPECT_EQ(make({.tile_mode = 31, .bits = 8, .pitch = 100, .height = 3}).size, 300u);
    // 1D, 1 byte: 8x8 alignment, then the pitch grows until a layer is a multiple of 256 bytes.
    EXPECT_EQ(make({.tile_mode = 13, .bits = 8, .pitch = 8, .height = 8}).levels[0].pitch, 32u);
    // 2D: level 2 (64x64) is smaller than the 128x64 macro tile and falls back to 1D tiling.
    const Layout l = make({.tile_mode = 14, .bits = 32, .pitch = 256, .height = 256, .levels = 3});
    EXPECT_TRUE(l.levels[1].macro);
    EXPECT_FALSE(l.levels[2].macro);
    EXPECT_EQ(l.levels[2].pitch, 64u);
    EXPECT_EQ(l.levels[2].offset, 256u * 256 * 4 + 128 * 128 * 4);
    // Depth with a 64-byte split: a 32-bit tile (256 bytes) is split, the macro mode is 1x4_16.
    const Layout d = make({.tile_mode = 0, .bits = 32, .pitch = 256, .height = 128});
    EXPECT_EQ(d.split, 64u);
    EXPECT_EQ(d.bank_height, 4u);
    EXPECT_EQ(d.macro_width, 256u);
    EXPECT_EQ(d.macro_height, 128u);
}

TEST(Tiling, Unsupported) {
    EXPECT_FALSE(layout({.tile_mode = 7}));   // PRT
    EXPECT_FALSE(layout({.tile_mode = 20}));  // thick
    EXPECT_FALSE(layout({.tile_mode = 10, .bits = 24}));
    EXPECT_FALSE(layout({.tile_mode = 10, .pitch = 0}));
    EXPECT_FALSE(layout({.tile_mode = 10, .levels = 17}));
    EXPECT_FALSE(layout({.tile_mode = 8, .bits = 128, .pitch = 16384, .height = 16384}));
}

TEST(Tiling, KnownOffsets) {
    // Display2DThin, 32 bits, pitch 1920: within a micro tile x0 x1 y0 x2 y1 y2 (4 bytes each);
    // x / 8 and y / 8 give the pipe (bits 8-10) and x / 64 and y / 8 the bank (bits 11-14).
    const Layout d = make({.tile_mode = 10, .bits = 32, .pitch = 1920, .height = 1080});
    EXPECT_EQ(tiled_offset(d, 0, 1, 0, 0), 4u);
    EXPECT_EQ(tiled_offset(d, 0, 0, 1, 0), 16u);
    EXPECT_EQ(tiled_offset(d, 0, 4, 0, 0), 32u);
    EXPECT_EQ(tiled_offset(d, 0, 8, 0, 0), 256u);   // pipe 1 (x3)
    EXPECT_EQ(tiled_offset(d, 0, 64, 0, 0), 2048u);  // bank 1 (x6)
    EXPECT_EQ(tiled_offset(d, 0, 0, 8, 0), 256u + (8u << 11));  // pipe 1 (y3), bank 8 (y3)
    // y = 64: the next row of macro tiles (15 of 256 bytes each), bank 3 (y6 in b0 and b1).
    EXPECT_EQ(tiled_offset(d, 0, 0, 64, 0), (3u << 11) + (15u << 15));
    // Thin1DThin, 32 bits, pitch 64: x0 y0 x1 y1 x2 y2, micro tiles of 256 bytes in rows of 8.
    const Layout t = make({.tile_mode = 13, .bits = 32, .pitch = 64, .height = 64});
    EXPECT_EQ(tiled_offset(t, 0, 1, 0, 0), 4u);
    EXPECT_EQ(tiled_offset(t, 0, 0, 1, 0), 8u);
    EXPECT_EQ(tiled_offset(t, 0, 8, 0, 0), 256u);
    EXPECT_EQ(tiled_offset(t, 0, 0, 8, 0), 2048u);
}

// Every element of every level and layer lands on its own element of the surface.
void check_surface(const Surface& s) {
    SCOPED_TRACE(testing::Message() << "mode " << s.tile_mode << ", " << s.bits << " bits");
    const Layout l = make(s);
    std::vector<bool> used(l.size / l.bytes);
    std::size_t count = 0;
    for (std::uint32_t i = 0; i < l.levels.size(); ++i) {
        const Level& lv = l.levels[i];
        for (std::uint32_t z = 0; z < s.layers; ++z) {
            for (std::uint32_t y = 0; y < lv.height; ++y) {
                for (std::uint32_t x = 0; x < lv.pitch; ++x, ++count) {
                    const std::uint64_t offset = tiled_offset(l, i, x, y, z);
                    ASSERT_EQ(offset % l.bytes, 0u);
                    ASSERT_LT(offset, l.size);
                    ASSERT_FALSE(used[offset / l.bytes]) << i << ' ' << x << ' ' << y << ' ' << z;
                    used[offset / l.bytes] = true;
                }
            }
        }
    }
    EXPECT_EQ(count, used.size());
}

TEST(Tiling, EveryModeIsABijection) {
    for (const std::uint32_t mode : supported) {
        for (std::uint32_t bits = 8; bits <= 128; bits *= 2) {
            check_surface({.tile_mode = mode, .bits = bits, .pitch = 200, .height = 100,
                           .layers = 2, .levels = 3});
        }
    }
    check_surface({.tile_mode = 14, .bits = 32, .pitch = 300, .height = 300, .layers = 2,
                   .levels = 2, .bank_swizzle = 5});
}

TEST(Tiling, CpuRoundTrip) {
    const Layout l = make({.tile_mode = 0, .bits = 32, .pitch = 300, .height = 200, .layers = 2,
                           .levels = 2});
    std::vector<std::byte> linear(l.size);
    for (std::size_t i = 0; i < linear.size(); ++i) {
        linear[i] = static_cast<std::byte>(i * 7 + i / 251);
    }
    std::vector<std::byte> tiled(l.size);
    std::vector<std::byte> back(l.size);
    tile(l, linear, tiled);
    detile(l, tiled, back);
    EXPECT_EQ(back, linear);
    // Element (5, 9) of layer 1, level 1, in both copies.
    const Level& lv = l.levels[1];
    const std::uint64_t at = lv.offset + ((1 * lv.height + 9) * lv.pitch + 5) * 4;
    EXPECT_EQ(tiled[tiled_offset(l, 1, 5, 9, 1)], linear[at]);
    EXPECT_THROW(detile(l, std::span(tiled).first(4), back), std::invalid_argument);
}

}  // namespace
