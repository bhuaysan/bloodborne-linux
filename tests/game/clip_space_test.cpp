// The clip space of the pipeline state against the eboot's own state code (docs/design/graphics.md,
// "Pipeline state (item 26, as built)"): the clip control of its rasterizer state and of a
// full-screen pass, and the screen viewport, written by the game's GNM writers on the mapped
// eboot and run through our parser into the register mirror.
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
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

// The rasterizer state builder 0x2178550 reads a description (fill mode, cull mode, front face
// counter-clockwise, depth bias, bias clamp, slope bias, then depth clip enable at +0x18) and
// writes PA_CL_CLIP_CNTL to dword 0, which its apply function writes with 0x1076b30 (0x26d33d1).
// It always sets DX_CLIP_SPACE_DEF, so z goes from 0 to w as in Vulkan; depth clip off sets both
// ZCLIP_*_DISABLE.
TEST_F(ClipSpace, RasterizerState) {
    for (const bool depth_clip : {true, false}) {
        std::array<std::uint32_t, 8> desc{};
        desc[6] = depth_clip;
        std::array<std::uint32_t, 9> state{};
        image().fn<int(std::uint32_t*, void*, const std::uint32_t*)>(0x2178550)(
            state.data(), nullptr, desc.data());
        EXPECT_EQ(state[0], depth_clip ? 1u << 19 : 1u << 19 | 3u << 26);
        const bb::vk::DrawState s = draw(state[0]);
        EXPECT_FALSE(s.key.negative_one_to_one);
        EXPECT_EQ(s.key.depth_clamp, !depth_clip);
    }
}

// The full-screen pass at 0x21d80b6 builds its PA_CL_CLIP_CNTL with 0x10850d0 and 0x1085130
// (clipping off: CLIP_DISABLE, and DX_CLIP_SPACE_DEF clear). Without clipping its depth stays the
// hardware's z, within 0-1 (checked in draw()), so z goes from 0 to w here too.
TEST_F(ClipSpace, FullScreenPass) {
    std::uint32_t clip = 0xffffffff;
    image().fn<void(std::uint32_t*)>(0x10850d0)(&clip);
    image().fn<void(std::uint32_t*, bool)>(0x1085130)(&clip, false);
    EXPECT_EQ(clip, 1u << 16);
    const bb::vk::DrawState s = draw(clip);
    EXPECT_TRUE(s.key.depth_clamp);
    EXPECT_FALSE(s.key.negative_one_to_one);
}

}  // namespace
