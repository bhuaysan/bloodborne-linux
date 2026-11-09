// The clip space of the pipeline state against the eboot's own state code (docs/design/graphics.md,
// "Pipeline state (item 26, as built)"): the clip control of a full-screen pass and the screen
// viewport, written by the game's GNM writers on the mapped eboot and run through our parser into
// the register mirror. The raster and blend state (#281, part 2) is built by the
// game's register value builders and written by its writers the same way (class RasterState).
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "gpu/pm4/parser.hpp"
#include "gpu/vk/pipeline_state.hpp"
#include "harness/eboot.hpp"

namespace {

// The command buffer object of the GNM writers (docs/formats/gnm.md, "Command buffer object").
struct CommandBuffer {
    std::uint32_t *begin, *end, *cursor;
    std::byte rest[0x28];
};

using Writer = void(CommandBuffer*, std::uint32_t);

class ClipSpace : public bb::test::EbootTest {
protected:
    // A triangle list (0x1073fe0), PA_CL_CLIP_CNTL `clip` (0x1076b30) and the screen viewport
    // helper 0x1096f60, which the full-screen pass below calls with (0, 0, width, height, zscale
    // 1, zoffset 0) at 0x21d81ba, written by the game into a command buffer; then the buffer run
    // through the parser, and the snapshot.
    bb::vk::DrawState draw(std::uint32_t clip) {
        std::array<std::uint32_t, 256> buffer{};
        CommandBuffer cb{};
        image().fn<void(CommandBuffer*, std::uint32_t*, std::uint32_t, void*, void*)>(0x1071270)(
            &cb, buffer.data(), sizeof buffer, nullptr, nullptr);
        image().fn<Writer>(0x1073fe0)(&cb, 4);
        image().fn<Writer>(0x1076b30)(&cb, clip);
        image().fn<void(CommandBuffer*, std::uint32_t, std::uint32_t, std::uint32_t,
                        std::uint32_t, float, float)>(0x1096f60)(&cb, 0, 0, 1920, 1080, 1, 0);
        bb::pm4::RegisterMirror regs;
        bb::pm4::Stream stream(std::span<const std::uint32_t>(cb.begin, cb.cursor));
        EXPECT_EQ(bb::pm4::Parser(regs).run(stream), bb::pm4::RunResult::done);
        std::string why;
        const auto s = bb::vk::draw_state(regs, false, why);
        EXPECT_TRUE(s) << why;
        // The whole screen, y down from the top, depth 0 to 1.
        const VkViewport v = s.value_or(bb::vk::DrawState{}).dynamic.viewport;
        EXPECT_EQ(std::vector<float>({v.x, v.y, v.width, v.height, v.minDepth, v.maxDepth}),
                  std::vector<float>({0, 1080, 1920, -1080, 0, 1}));
        return s.value_or(bb::vk::DrawState{});
    }
};

// The full-screen pass at 0x21d80b6 builds its PA_CL_CLIP_CNTL with 0x10850d0 and 0x1085130
// (clipping off: CLIP_DISABLE, and DX_CLIP_SPACE_DEF clear). Without clipping its depth stays the
// hardware's z, within 0-1 (checked in draw()): zoffset - zscale is below 0, so z goes from 0 to w.
TEST_F(ClipSpace, FullScreenPass) {
    std::uint32_t clip = 0xffffffff;
    image().fn<void(std::uint32_t*)>(0x10850d0)(&clip);
    image().fn<void(std::uint32_t*, bool)>(0x1085130)(&clip, false);
    EXPECT_EQ(clip, 1u << 16);
    const bb::vk::DrawState s = draw(clip);
    EXPECT_TRUE(s.key.depth_clamp);
    EXPECT_FALSE(s.key.negative_one_to_one);
}

// The game's register value builders and writers for the raster and blend state, run on the
// mapped eboot; the snapshot has to read back what the game put in.
class RasterState : public bb::test::EbootTest {
protected:
    // `sets` are (writer address, value); a non-zero `blend0` is written to CB_BLEND0_CONTROL
    // (SET_CONTEXT_REG 0xa1e0) as a raw packet.
    bb::vk::DrawState draw(const std::vector<std::pair<std::uint32_t, std::uint32_t>>& sets,
                           std::uint32_t blend0 = 0) {
        std::array<std::uint32_t, 256> buffer{};
        CommandBuffer cb{};
        image().fn<void(CommandBuffer*, std::uint32_t*, std::uint32_t, void*, void*)>(0x1071270)(
            &cb, buffer.data(), sizeof buffer, nullptr, nullptr);
        image().fn<Writer>(0x1073fe0)(&cb, 4);
        image().fn<Writer>(0x1076b30)(&cb, 1u << 19);
        image().fn<void(CommandBuffer*, std::uint32_t, std::uint32_t, std::uint32_t,
                        std::uint32_t, float, float)>(0x1096f60)(&cb, 0, 0, 1920, 1080, 1, 0);
        for (const auto& [writer, value] : sets) {
            image().fn<Writer>(writer)(&cb, value);
        }
        if (blend0 != 0) {
            for (const std::uint32_t word : {0xc0016900u, 0x1e0u, blend0}) {
                *cb.cursor++ = word;
            }
        }
        bb::pm4::RegisterMirror regs;
        bb::pm4::Stream stream(std::span<const std::uint32_t>(cb.begin, cb.cursor));
        EXPECT_EQ(bb::pm4::Parser(regs).run(stream), bb::pm4::RunResult::done);
        std::string why;
        const auto s = bb::vk::draw_state(regs, false, why);
        EXPECT_TRUE(s) << why;
        return s.value_or(bb::vk::DrawState{});
    }

    // PA_SU_SC_MODE_CNTL as the game builds it: the default 0x1084f40 (solid), cull mode 0x1084f50
    // (0 none, 1 front, 2 back), FACE 0x1084f60 and the polygon types 0x1084f70 (front, back).
    std::uint32_t mode(std::uint32_t cull, bool face, std::uint32_t front, std::uint32_t back) {
        std::uint32_t v = 0xffffffff;
        image().fn<void(std::uint32_t*)>(0x1084f40)(&v);
        image().fn<void(std::uint32_t*, std::uint32_t)>(0x1084f50)(&v, cull);
        image().fn<void(std::uint32_t*, bool)>(0x1084f60)(&v, face);
        image().fn<void(std::uint32_t*, std::uint32_t, std::uint32_t)>(0x1084f70)(&v, front, back);
        return v;
    }
};

TEST_F(RasterState, CullFaceAndPolygonMode) {
    const std::uint32_t solid = mode(0, false, 2, 2);
    EXPECT_EQ(solid, 0x240u);
    bb::vk::DrawState s = draw({{0x1076e00, solid}});
    EXPECT_EQ(s.dynamic.cull_mode, VK_CULL_MODE_NONE);
    EXPECT_EQ(s.dynamic.front_face, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    EXPECT_EQ(s.key.polygon_mode, VK_POLYGON_MODE_FILL);
    s = draw({{0x1076e00, mode(1, true, 2, 2)}});
    EXPECT_EQ(s.dynamic.cull_mode, VK_CULL_MODE_FRONT_BIT);
    EXPECT_EQ(s.dynamic.front_face, VK_FRONT_FACE_CLOCKWISE);
    s = draw({{0x1076e00, mode(2, false, 1, 1)}});  // wireframe
    EXPECT_EQ(s.dynamic.cull_mode, VK_CULL_MODE_BACK_BIT);
    EXPECT_EQ(s.key.polygon_mode, VK_POLYGON_MODE_LINE);
    EXPECT_EQ(draw({{0x1076e00, mode(0, false, 0, 0)}}).key.polygon_mode,
              VK_POLYGON_MODE_POINT);
}

// CB_BLEND<n>_CONTROL as the game's builders put it together: colour factors and function
// (0x1084ed0: source bits 0-4, function 5-7, destination 8-12), alpha ones (0x1084ef0: bits 16-20,
// 21-23, 24-28), SEPARATE_ALPHA_BLEND (0x1084f20, bit 29) and ENABLE (0x1084eb0, bit 30).
TEST_F(RasterState, BlendControl) {
    std::uint32_t v = 0;
    image().fn<void(std::uint32_t*, std::uint32_t, std::uint32_t, std::uint32_t)>(0x1084ed0)(
        &v, 4, 0, 5);  // src alpha, add, one minus src alpha
    image().fn<void(std::uint32_t*, std::uint32_t, std::uint32_t, std::uint32_t)>(0x1084ef0)(
        &v, 1, 4, 13);  // one, reverse subtract, constant colour
    image().fn<void(std::uint32_t*, bool)>(0x1084f20)(&v, true);
    image().fn<void(std::uint32_t*, bool)>(0x1084eb0)(&v, true);
    const bb::vk::BlendAttachment b = draw({}, v).key.blend[0];
    EXPECT_TRUE(b.enable);
    EXPECT_EQ(std::vector({b.src_color, b.dst_color, b.src_alpha, b.dst_alpha}),
              std::vector({VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                           VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_CONSTANT_COLOR}));
    EXPECT_EQ(std::pair(b.color_op, b.alpha_op),
              std::pair(VK_BLEND_OP_ADD, VK_BLEND_OP_REVERSE_SUBTRACT));
}

}  // namespace
