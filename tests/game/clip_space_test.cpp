// The clip space of the pipeline state against the eboot's own state code (docs/design/graphics.md,
// "Pipeline state (item 26, as built)"): the clip control of a full-screen pass and the screen
// viewport, written by the game's GNM writers on the mapped eboot and run through our parser into
// the register mirror. The rasterizer state builder 0x2178550 follows with the raster state.
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

}  // namespace
