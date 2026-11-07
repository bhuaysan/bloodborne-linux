#include <bit>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "gpu/vk/pipeline_state.hpp"
#include "si_ci_vi_merged_offset.h"

namespace {

using namespace bb::vk;
using bb::pm4::RegisterMirror;
namespace amd = Pal::Gfx6::Chip;

void set(RegisterMirror& regs, std::uint32_t reg, std::uint32_t value) {
    for (const auto space : {bb::pm4::RegSpace::sh, bb::pm4::RegSpace::context,
                             bb::pm4::RegSpace::uconfig}) {
        const auto range = bb::pm4::space_range(space);
        if (reg >= range.base && reg < range.base + range.size) {
            regs.write(space, reg - range.base, {&value, 1});
        }
    }
}

void set(RegisterMirror& regs, std::uint32_t reg, float value) {
    set(regs, reg, std::bit_cast<std::uint32_t>(value));
}

// A triangle list in DX clip space with the viewport that the game's screen viewport helper
// writes for 1920x1080 (tests/game/clip_space_test.cpp).
RegisterMirror draw_registers() {
    RegisterMirror regs;
    set(regs, amd::mmVGT_PRIMITIVE_TYPE__CI__VI, 4u);
    set(regs, amd::mmPA_CL_CLIP_CNTL, 1u << 19);
    set(regs, amd::mmPA_CL_VTE_CNTL, 0x43fu);
    set(regs, amd::mmPA_CL_VPORT_XSCALE, 960.0f);
    set(regs, amd::mmPA_CL_VPORT_XOFFSET, 960.0f);
    set(regs, amd::mmPA_CL_VPORT_YSCALE, -540.0f);
    set(regs, amd::mmPA_CL_VPORT_YOFFSET, 540.0f);
    set(regs, amd::mmPA_CL_VPORT_ZSCALE, 1.0f);
    return regs;
}

DrawState snapshot(const RegisterMirror& regs, bool quad_control = false) {
    std::string why;
    const auto s = draw_state(regs, quad_control, why);
    EXPECT_TRUE(s) << why;
    return s.value_or(DrawState{});
}

std::string failure(const RegisterMirror& regs) {
    std::string why;
    return draw_state(regs, false, why) ? "no failure" : why;
}

TEST(PipelineState, Topology) {
    EXPECT_EQ(topology(3), VK_PRIMITIVE_TOPOLOGY_LINE_STRIP);
    EXPECT_EQ(topology(13), VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY);
    EXPECT_EQ(topology(21), VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN);  // polygon
    // None, patch, rect list, line loop, quad list and strip, past the end.
    for (const std::uint32_t t : {0u, 9u, 17u, 18u, 19u, 20u, 22u}) {
        EXPECT_EQ(topology(t), std::nullopt) << t;
    }
}

TEST(PipelineState, Stages) {
    RegisterMirror regs = draw_registers();
    set(regs, amd::mmSPI_SHADER_PGM_LO_VS, 0x1234u);  // address >> 8
    set(regs, amd::mmSPI_SHADER_PGM_RSRC2_VS, 4u << 1);  // four user SGPRs
    set(regs, amd::mmSPI_SHADER_USER_DATA_VS_0, 0x5000u);
    set(regs, amd::mmSPI_SHADER_USER_DATA_VS_0 + 1, 0x9u);
    DrawState s = snapshot(regs);
    EXPECT_EQ(s.key.vs.code_address, 0x123400u);
    EXPECT_EQ(s.key.vs.fetch_address, 0x9'0000'5000u);
    EXPECT_EQ(s.key.vs.key.push_offset, bb::gcn::vertex_push_offset);
    EXPECT_FALSE(s.key.ps);

    set(regs, amd::mmSPI_SHADER_PGM_RSRC2_VS, 1u << 1);  // one user SGPR: no fetch address
    set(regs, amd::mmSPI_SHADER_PGM_LO_PS, 0x10u);
    set(regs, amd::mmSPI_PS_INPUT_ADDR, 3u);
    set(regs, amd::mmSPI_PS_INPUT_CNTL_0 + 31, 0x20u);
    s = snapshot(regs, true);
    EXPECT_EQ(s.key.vs.fetch_address, 0u);
    ASSERT_TRUE(s.key.ps);
    EXPECT_EQ(s.key.ps->code_address, 0x1000u);
    EXPECT_EQ(s.key.ps->key.ps_input_addr, 3u);
    EXPECT_EQ(s.key.ps->key.ps_input_cntl[31], 0x20u);
    EXPECT_TRUE(s.key.ps->key.quad_control);
    EXPECT_EQ(s.key.ps->key.push_offset, bb::gcn::pixel_push_offset);
    EXPECT_EQ(s.key, snapshot(regs, true).key);
    EXPECT_NE(s.key, snapshot(regs).key);

    set(regs, amd::mmVGT_SHADER_STAGES_EN, 1u << 5);  // GS
    EXPECT_EQ(failure(regs), "VGT_SHADER_STAGES_EN 0x20: only VS and PS are supported yet");
    set(regs, amd::mmVGT_SHADER_STAGES_EN, 0u);
    set(regs, amd::mmVGT_PRIMITIVE_TYPE__CI__VI, 17u);  // rect list
    EXPECT_EQ(failure(regs), "VGT_PRIMITIVE_TYPE 17 is not supported yet");
}

// The hardware's viewport transform. The game's own values are checked in
// tests/game/clip_space_test.cpp.
TEST(PipelineState, ClipSpace) {
    RegisterMirror regs = draw_registers();
    DrawState s = snapshot(regs);
    EXPECT_FALSE(s.key.negative_one_to_one || s.key.depth_clamp);
    const VkViewport v = s.dynamic.viewport;  // y flipped: NDC y = 1 is the top
    EXPECT_EQ(std::vector<float>({v.x, v.y, v.width, v.height, v.minDepth, v.maxDepth}),
              std::vector<float>({0, 1080, 1920, -1080, 0, 1}));

    // OpenGL's clip space with depth range 0-1: z from -w to w, scale and offset 0.5.
    set(regs, amd::mmPA_CL_CLIP_CNTL, 0u);
    set(regs, amd::mmPA_CL_VPORT_ZSCALE, 0.5f);
    set(regs, amd::mmPA_CL_VPORT_ZOFFSET, 0.5f);
    s = snapshot(regs);
    EXPECT_TRUE(s.key.negative_one_to_one);
    EXPECT_EQ(std::pair(s.dynamic.viewport.minDepth, s.dynamic.viewport.maxDepth),
              std::pair(0.f, 1.f));
    // Without clipping the transform from 0 to w gives the hardware's 0.5 z + 0.5 within 0-1.
    for (const std::uint32_t clip : {1u << 16, 1u << 26, 1u << 27}) {
        set(regs, amd::mmPA_CL_CLIP_CNTL, clip);
        s = snapshot(regs);
        EXPECT_TRUE(s.key.depth_clamp && !s.key.negative_one_to_one) << clip;
        EXPECT_EQ(std::pair(s.dynamic.viewport.minDepth, s.dynamic.viewport.maxDepth),
                  std::pair(0.5f, 1.f));
    }

    set(regs, amd::mmPA_CL_VPORT_ZSCALE, 2.0f);  // clamped to 0-1; no x scale or offset: 1, 0
    set(regs, amd::mmPA_CL_VTE_CNTL, 0x43cu);
    const VkViewport w = *viewport(regs, true);
    EXPECT_EQ(std::vector<float>({w.x, w.width, w.minDepth, w.maxDepth}),
              std::vector<float>({-1, 2, 0, 1}));
    set(regs, amd::mmPA_CL_VTE_CNTL, 0x13fu);  // positions already in screen space
    EXPECT_EQ(failure(regs), "PA_CL_VTE_CNTL 0x13f is not supported yet");
    set(regs, amd::mmPA_CL_VTE_CNTL, 0x43fu);
    set(regs, amd::mmPA_CL_VPORT_YSCALE, 0.0f);
    EXPECT_EQ(failure(regs), "viewport 0 has a zero scale");
}

}  // namespace
