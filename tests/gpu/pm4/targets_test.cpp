#include <cstdint>
#include <initializer_list>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "gpu/pm4/targets.hpp"

namespace {

using namespace bb::pm4;

// Runs packets the way the parser will: SET_CONTEXT_REG fills the mirror, and every packet goes
// to the hint tracker.
struct Stream {
    RegisterMirror regs;
    ExtentHints hints;

    void run(std::uint8_t opcode, std::initializer_list<std::uint32_t> body) {
        const std::vector<std::uint32_t> dwords(body);
        const Packet packet{type3_header(opcode, static_cast<std::uint32_t>(dwords.size())),
                            dwords};
        if (opcode == IT_SET_CONTEXT_REG) {
            regs.write(RegSpace::context, dwords[0], std::span(dwords).subspan(1));
        }
        hints.observe(packet);
    }
};

// The packets of the game's colour target setter (0x10738b0) for `slot`: CB_COLOR<n>_BASE to
// FMASK_SLICE, where the seventh dword (the unused register 0xA31E on this GPU) repeats the hint,
// then the hint NOP.
void color_setup(Stream& s, unsigned slot, std::uint32_t hint) {
    s.run(IT_SET_CONTEXT_REG, {0x318 + 15 * slot, 0x123456, 239, 32639, 0x2000,
                               10 << 2 | 6 << 8 | 1 << 13, 0x8000 | 13, hint, 0x654321, 7, 0, 0});
    s.run(IT_NOP, {hint});
}

TEST(Targets, ColorTargetWithTheGamesHint) {
    Stream s;
    color_setup(s, 2, 1080u << 16 | 1920);
    s.run(IT_SET_CONTEXT_REG, {0x323 + 15 * 2, 0xFF00FF00, 0x11});  // CLEAR_WORD0/1
    const auto t = color_target(s.regs, s.hints, 2);
    ASSERT_TRUE(t);
    EXPECT_EQ(t->address, 0x12345600u);
    EXPECT_EQ(t->pitch, 1920u);   // (239 + 1) * 8
    EXPECT_EQ(t->height, 1088u);  // (32639 + 1) * 64 / 1920, rounded up to tiles
    EXPECT_EQ(t->extent, (Extent{1920, 1080}));
    EXPECT_EQ(t->base_slice, 0u);
    EXPECT_EQ(t->slices, 2u);  // VIEW 0x2000: SLICE_MAX 1
    EXPECT_EQ(t->format, 10u);  // COLOR_8_8_8_8, NUMBER_SRGB, FAST_CLEAR
    EXPECT_EQ(t->number_type, 6u);
    EXPECT_EQ(t->comp_swap, 0u);
    EXPECT_TRUE(t->fast_clear);
    EXPECT_FALSE(t->compression);
    EXPECT_FALSE(t->linear_general);
    EXPECT_EQ(t->tile_mode, 13u);  // ATTRIB: TILE_MODE_INDEX 13, NUM_FRAGMENTS 1
    EXPECT_EQ(t->samples, 2u);
    EXPECT_EQ(t->cmask_address, 0x65432100u);
    EXPECT_EQ(t->clear_word, 0x11FF00FF00u);
    EXPECT_FALSE(color_target(s.regs, s.hints, 1));  // never set
    EXPECT_FALSE(color_target(s.regs, s.hints, 8));
}

TEST(Targets, ColorTargetWithoutAHintUsesTheTileSize) {
    Stream s;
    color_setup(s, 0, 720u << 16 | 1280);
    // A new setup without the NOP drops the old hint.
    s.run(IT_SET_CONTEXT_REG, {0x318, 0x1000, 159, 14399, 0, 10 << 2});
    s.run(IT_SET_SH_REG, {0x0, 0});
    s.run(IT_NOP, {720u << 16 | 1280});  // not right after the setup
    const auto t = color_target(s.regs, s.hints, 0);
    ASSERT_TRUE(t);
    EXPECT_EQ(t->extent, (Extent{1280, 720}));  // (159 + 1) * 8, (14399 + 1) * 64 / 1280
    EXPECT_EQ(s.hints.color(0), Extent{});

    // A tagged NOP is no hint; nor is a zero size.
    color_setup(s, 0, 0);
    EXPECT_EQ(s.hints.color(0), Extent{});
    s.run(IT_SET_CONTEXT_REG, {0x318, 0x1000});
    s.run(IT_NOP, {0x68750004});
    EXPECT_EQ(s.hints.color(0), Extent{});

    s.run(IT_SET_CONTEXT_REG, {0x31C, 0});  // INFO 0: format invalid
    EXPECT_FALSE(color_target(s.regs, s.hints, 0));
}

// The packets of the game's depth target setter (0x10739b0): DB_Z_INFO to DB_DEPTH_SLICE,
// DB_DEPTH_INFO, DB_DEPTH_VIEW, DB_HTILE_DATA_BASE and DB_HTILE_SURFACE, then the hint NOP.
void depth_setup(Stream& s, std::uint32_t z_info, std::uint32_t stencil_info,
                 std::uint32_t hint) {
    s.run(IT_SET_CONTEXT_REG,
          {0x10, z_info, stencil_info, 0x100, 0x200, 0x101, 0x201, 159 | 89 << 11, 0});
    s.run(IT_SET_CONTEXT_REG, {0x0F, 0});
    s.run(IT_SET_CONTEXT_REG, {0x02, 5 | 7 << 13});
    s.run(IT_SET_CONTEXT_REG, {0x05, 0x777});
    s.run(IT_SET_CONTEXT_REG, {0x2AF, 0});
    s.run(IT_NOP, {hint});
}

TEST(Targets, DepthTargetWithTheGamesHint) {
    Stream s;
    depth_setup(s, 3 | 1 << 2 | 6 << 20 | 1u << 29, 1, 718u << 16 | 1278);
    s.run(IT_SET_CONTEXT_REG, {0x0, 3});           // DB_RENDER_CONTROL: both clears
    s.run(IT_SET_CONTEXT_REG, {0x0A, 0x1FF, 0x3F000000});  // DB_STENCIL_CLEAR, DB_DEPTH_CLEAR
    const auto t = depth_target(s.regs, s.hints);
    ASSERT_TRUE(t);
    EXPECT_EQ(t->z_format, 3u);
    EXPECT_TRUE(t->stencil);
    EXPECT_EQ(t->z_address, 0x10000u);
    EXPECT_EQ(t->stencil_address, 0x20000u);
    EXPECT_EQ(t->z_write_address, 0x10100u);
    EXPECT_EQ(t->stencil_write_address, 0x20100u);
    EXPECT_EQ(t->pitch, 1280u);
    EXPECT_EQ(t->height, 720u);
    EXPECT_EQ(t->extent, (Extent{1278, 718}));
    EXPECT_EQ(t->base_slice, 5u);
    EXPECT_EQ(t->slices, 8u);
    EXPECT_EQ(t->tile_mode, 6u);
    EXPECT_EQ(t->samples, 2u);
    EXPECT_EQ(t->htile_address, 0x77700u);
    EXPECT_TRUE(t->clear_depth);
    EXPECT_TRUE(t->clear_stencil);
    EXPECT_EQ(t->depth_clear, 0.5f);
    EXPECT_EQ(t->stencil_clear, 0xFFu);
}

TEST(Targets, DepthTargetParts) {
    Stream s;
    depth_setup(s, 1, 0, 0);  // Z16 without stencil, no HTILE, no hint
    auto t = depth_target(s.regs, s.hints);
    ASSERT_TRUE(t);
    EXPECT_EQ(t->z_format, 1u);
    EXPECT_FALSE(t->stencil);
    EXPECT_EQ(t->htile_address, 0u);
    EXPECT_EQ(t->extent, (Extent{1280, 720}));

    depth_setup(s, 0, 1, 0);  // stencil only
    t = depth_target(s.regs, s.hints);
    ASSERT_TRUE(t);
    EXPECT_EQ(t->z_format, 0u);
    EXPECT_TRUE(t->stencil);

    s.run(IT_SET_CONTEXT_REG, {0x13, 0});  // no stencil read base either
    EXPECT_FALSE(depth_target(s.regs, s.hints));
}

}  // namespace
