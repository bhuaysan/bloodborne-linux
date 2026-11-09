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

TEST(PipelineState, Tables) {
    // CB_BLENDn_CONTROL's factors: 11 and 12 are undefined, 15-18 are the dual-source ones.
    EXPECT_EQ(blend_factor(10), VK_BLEND_FACTOR_SRC_ALPHA_SATURATE);
    EXPECT_EQ(blend_factor(13), VK_BLEND_FACTOR_CONSTANT_COLOR);
    EXPECT_EQ(blend_factor(16), VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR);
    EXPECT_EQ(blend_factor(19), VK_BLEND_FACTOR_CONSTANT_ALPHA);
    EXPECT_EQ(blend_factor(20), VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA);
    for (const std::uint32_t f : {11u, 12u, 21u, 31u}) {
        EXPECT_EQ(blend_factor(f), std::nullopt) << f;
    }
    EXPECT_EQ(blend_op(2), VK_BLEND_OP_MIN);
    EXPECT_EQ(blend_op(4), VK_BLEND_OP_REVERSE_SUBTRACT);
    EXPECT_EQ(blend_op(5), std::nullopt);
}

TEST(PipelineState, Raster) {
    RegisterMirror regs = draw_registers();
    DrawState s = snapshot(regs);
    EXPECT_EQ(s.dynamic.cull_mode, VK_CULL_MODE_NONE);
    EXPECT_EQ(s.dynamic.front_face, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    EXPECT_EQ(s.key.polygon_mode, VK_POLYGON_MODE_FILL);
    const PipelineKey solid = s.key;

    set(regs, amd::mmPA_SU_SC_MODE_CNTL, 1u | 4u | 2u << 5 | 2u << 8);  // cull front, clockwise
    s = snapshot(regs);
    EXPECT_EQ(s.dynamic.cull_mode, VK_CULL_MODE_FRONT_BIT);
    EXPECT_EQ(s.dynamic.front_face, VK_FRONT_FACE_CLOCKWISE);
    EXPECT_EQ(s.key, solid);  // cull and face are dynamic
    set(regs, amd::mmPA_SU_SC_MODE_CNTL, 2u);
    EXPECT_EQ(snapshot(regs).dynamic.cull_mode, VK_CULL_MODE_BACK_BIT);
    set(regs, amd::mmPA_SU_SC_MODE_CNTL, 3u);
    EXPECT_EQ(snapshot(regs).dynamic.cull_mode, VK_CULL_MODE_FRONT_AND_BACK);

    // POLY_MODE with both types lines or points; the types are ignored without POLY_MODE.
    set(regs, amd::mmPA_SU_SC_MODE_CNTL, 1u << 3 | 1u << 5 | 1u << 8);
    EXPECT_EQ(snapshot(regs).key.polygon_mode, VK_POLYGON_MODE_LINE);
    EXPECT_NE(snapshot(regs).key, solid);
    set(regs, amd::mmPA_SU_SC_MODE_CNTL, 1u << 3);
    EXPECT_EQ(snapshot(regs).key.polygon_mode, VK_POLYGON_MODE_POINT);
    set(regs, amd::mmPA_SU_SC_MODE_CNTL, 1u << 5);
    EXPECT_EQ(snapshot(regs).key.polygon_mode, VK_POLYGON_MODE_FILL);
    set(regs, amd::mmPA_SU_SC_MODE_CNTL, 1u << 3 | 1u << 5 | 2u << 8);
    EXPECT_NE(failure(regs).find("polygon types differ"), std::string::npos);
    set(regs, amd::mmPA_SU_SC_MODE_CNTL, 1u << 3 | 3u << 5 | 3u << 8);
    EXPECT_NE(failure(regs).find("polygon types differ"), std::string::npos);
    set(regs, amd::mmPA_SU_SC_MODE_CNTL, 2u << 3 | 2u << 5 | 2u << 8);  // reserved POLY_MODE
    EXPECT_NE(failure(regs).find("POLY_MODE is reserved"), std::string::npos);
}

TEST(PipelineState, Blend) {
    RegisterMirror regs = draw_registers();
    EXPECT_EQ(snapshot(regs).key.blend[0], BlendAttachment{.write_mask = 0});  // CB_TARGET_MASK 0
    set(regs, amd::mmCB_TARGET_MASK, 0x000ffff7u);
    set(regs, amd::mmCB_BLEND_RED, 0.25f);
    set(regs, amd::mmCB_BLEND_ALPHA, 1.0f);
    // Target 1: src alpha, one minus src alpha, add; alpha not separate, so the same.
    const std::uint32_t alpha_blend = 4u | 0u << 5 | 5u << 8 | 1u << 30;
    set(regs, amd::mmCB_BLEND0_CONTROL + 1, alpha_blend);
    // Target 2: separate alpha; colour max of one and one, alpha reverse subtract of src colour and constant colour.
    set(regs, amd::mmCB_BLEND0_CONTROL + 2,
        1u | 3u << 5 | 1u << 8 | 2u << 16 | 4u << 21 | 13u << 24 | 1u << 29 | 1u << 30);
    // Target 3: disabled, with factors set.
    set(regs, amd::mmCB_BLEND0_CONTROL + 3, alpha_blend & ~(1u << 30));
    // Target 4: enabled but bypassed by CB_COLOR4_INFO.BLEND_BYPASS.
    set(regs, amd::mmCB_BLEND0_CONTROL + 4, alpha_blend);
    set(regs, amd::mmCB_COLOR0_INFO + 4 * (amd::mmCB_COLOR1_INFO - amd::mmCB_COLOR0_INFO),
        1u << 16);
    const DrawState s = snapshot(regs);
    const auto& b = s.key.blend;
    EXPECT_EQ(b[0].write_mask, 7);
    EXPECT_FALSE(b[0].enable);
    EXPECT_EQ(b[1].write_mask, 0xf);
    EXPECT_EQ(std::vector({b[1].src_color, b[1].dst_color, b[1].src_alpha, b[1].dst_alpha}),
              std::vector({VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                           VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA}));
    EXPECT_TRUE(b[1].enable);
    EXPECT_EQ(b[2].write_mask, 0xf);
    EXPECT_EQ(std::vector({b[2].src_color, b[2].dst_color, b[2].src_alpha, b[2].dst_alpha}),
              std::vector({VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR,
                           VK_BLEND_FACTOR_CONSTANT_COLOR}));
    EXPECT_EQ(std::pair(b[2].color_op, b[2].alpha_op),
              std::pair(VK_BLEND_OP_MAX, VK_BLEND_OP_REVERSE_SUBTRACT));
    // Disabled and bypassed: the default factors, so equal keys, with the mask of the nibble.
    EXPECT_EQ(b[3], BlendAttachment{});
    EXPECT_EQ(b[4], BlendAttachment{});
    EXPECT_EQ(s.dynamic.blend_constants, (std::array<float, 4>{0.25f, 0, 0, 1}));

    set(regs, amd::mmCB_BLEND0_CONTROL + 5, 1u | 15u << 8 | 1u << 30);  // src1 colour
    // Slot 5 is unbound (mask nibble 0): its stale control is not decoded and the key is unchanged.
    EXPECT_EQ(snapshot(regs).key.blend[5], BlendAttachment{.write_mask = 0});
    set(regs, amd::mmCB_TARGET_MASK, 0x00fffff7u);
    EXPECT_EQ(failure(regs), "target 5: dual-source blend factor 15");
    set(regs, amd::mmCB_BLEND0_CONTROL + 5, 1u | 11u << 8 | 1u << 30);
    EXPECT_NE(failure(regs).find("target 5: CB_BLEND_CONTROL"), std::string::npos);
    set(regs, amd::mmCB_BLEND0_CONTROL + 5, 1u | 5u << 5 | 1u << 30);  // function 5
    EXPECT_NE(failure(regs).find("not supported"), std::string::npos);
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
    // Without clipping only the clamp range differs: zoffset +- zscale lies within 0-1, so z from
    // -w to w clamps to 0-1 like the hardware (z from 0 to w would clamp to 0.5-1).
    for (const std::uint32_t clip : {1u << 16, 1u << 26, 1u << 27}) {
        set(regs, amd::mmPA_CL_CLIP_CNTL, clip);
        s = snapshot(regs);
        EXPECT_TRUE(s.key.depth_clamp && s.key.negative_one_to_one) << clip;
        EXPECT_EQ(std::pair(s.dynamic.viewport.minDepth, s.dynamic.viewport.maxDepth),
                  std::pair(0.f, 1.f));
    }
    // Reversed depth (zscale -0.5) also lies within 0-1.
    set(regs, amd::mmPA_CL_VPORT_ZSCALE, -0.5f);
    s = snapshot(regs);
    EXPECT_TRUE(s.key.depth_clamp && s.key.negative_one_to_one);
    EXPECT_EQ(std::pair(s.dynamic.viewport.minDepth, s.dynamic.viewport.maxDepth),
              std::pair(1.f, 0.f));
    // zoffset - zscale below 0 (the game's zscale 1, zoffset 0): z from 0 to w keeps 0-1.
    set(regs, amd::mmPA_CL_VPORT_ZSCALE, 1.0f);
    set(regs, amd::mmPA_CL_VPORT_ZOFFSET, 0.0f);
    s = snapshot(regs);
    EXPECT_TRUE(s.key.depth_clamp && !s.key.negative_one_to_one);
    EXPECT_EQ(std::pair(s.dynamic.viewport.minDepth, s.dynamic.viewport.maxDepth),
              std::pair(0.f, 1.f));

    set(regs, amd::mmPA_CL_VPORT_ZSCALE, 2.0f);  // clamped to 0-1; no x scale or offset: 1, 0
    set(regs, amd::mmPA_CL_VTE_CNTL, 0x43cu);
    const VkViewport w = snapshot(regs).dynamic.viewport;
    EXPECT_EQ(std::vector<float>({w.x, w.width, w.minDepth, w.maxDepth}),
              std::vector<float>({-1, 2, 0, 1}));
    set(regs, amd::mmPA_CL_VTE_CNTL, 0x13fu);  // positions already in screen space
    EXPECT_EQ(failure(regs), "PA_CL_VTE_CNTL 0x13f is not supported yet");
    set(regs, amd::mmPA_CL_VTE_CNTL, 0x43fu);
    set(regs, amd::mmPA_CL_VPORT_XSCALE, -960.0f);  // Vulkan allows only a negative height
    EXPECT_EQ(failure(regs), "viewport 0 has a negative x scale");
    set(regs, amd::mmPA_CL_VPORT_XSCALE, 960.0f);
    set(regs, amd::mmPA_CL_VPORT_YSCALE, 0.0f);
    EXPECT_EQ(failure(regs), "viewport 0 has a zero scale");
}

}  // namespace
