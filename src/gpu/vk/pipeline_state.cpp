// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/video_core/renderer_vulkan/liverpool_to_vk.cpp @0d5f2739d2544d2a35c73e354cba7a32b7a81bea
// Source: shadPS4 src/video_core/renderer_vulkan/vk_rasterizer.cpp @0d5f2739d2544d2a35c73e354cba7a32b7a81bea
// Source: shadPS4 src/video_core/amdgpu/regs_primitive.h @0d5f2739d2544d2a35c73e354cba7a32b7a81bea
// Changed for bloodborne-linux, 2026-11-07: the topology table without fallbacks; the viewport of
// UpdateViewportScissorState for viewport 0 of our register mirror, also when clipping is off.

#include "gpu/vk/pipeline_state.hpp"

#include <algorithm>
#include <bit>
#include <format>

#include "si_ci_vi_merged_offset.h"

namespace bb::vk {

namespace {

namespace amd = Pal::Gfx6::Chip;

constexpr std::uint32_t field(std::uint32_t value, unsigned first, unsigned bits) {
    return value >> first & ((1u << bits) - 1);
}

std::optional<DrawState> fail(std::string& why, std::string text) {
    why = std::move(text);
    return std::nullopt;
}

gcn::ShaderKey vertex_key(const pm4::RegisterMirror& regs) {
    return {.stage = gcn::Stage::vertex,
            .pgm_rsrc1 = regs.rsrc1(pm4::Stage::vs),
            .pgm_rsrc2 = regs.rsrc2(pm4::Stage::vs),
            .instance_step_rate = {regs.get(amd::mmVGT_INSTANCE_STEP_RATE_0),
                                   regs.get(amd::mmVGT_INSTANCE_STEP_RATE_1)},
            .push_offset = gcn::vertex_push_offset};
}

gcn::ShaderKey pixel_key(const pm4::RegisterMirror& regs, bool quad_control) {
    gcn::ShaderKey key{.stage = gcn::Stage::pixel,
                       .pgm_rsrc1 = regs.rsrc1(pm4::Stage::ps),
                       .pgm_rsrc2 = regs.rsrc2(pm4::Stage::ps),
                       .ps_input_ena = regs.get(amd::mmSPI_PS_INPUT_ENA),
                       .ps_input_addr = regs.get(amd::mmSPI_PS_INPUT_ADDR),
                       .col_format = regs.get(amd::mmSPI_SHADER_COL_FORMAT),
                       .z_format = regs.get(amd::mmSPI_SHADER_Z_FORMAT),
                       .db_shader_control = regs.get(amd::mmDB_SHADER_CONTROL),
                       .quad_control = quad_control,
                       .push_offset = gcn::pixel_push_offset};
    for (std::uint32_t n = 0; n < key.ps_input_cntl.size(); ++n) {
        key.ps_input_cntl[n] = regs.get(amd::mmSPI_PS_INPUT_CNTL_0 + n);
    }
    return key;
}

}  // namespace

std::optional<VkPrimitiveTopology> topology(std::uint32_t prim_type) {
    switch (prim_type) {
    case 1: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case 2: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case 3: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case 4: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case 5:                                              // triangle fan
    case 21: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;  // polygon
    case 6: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case 10: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
    case 11: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
    case 12: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
    case 13: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
    default: return std::nullopt;  // none, patches, rect list, line loop, quad list and strip
    }
}

std::optional<VkViewport> viewport(const pm4::RegisterMirror& regs, bool negative_one_to_one) {
    // PA_CL_VTE_CNTL bits 0-5 enable the x scale and offset, then y, then z.
    const std::uint32_t vte = regs.get(amd::mmPA_CL_VTE_CNTL);
    const auto get = [&](unsigned bit, std::uint32_t reg, float disabled) {
        return field(vte, bit, 1) != 0 ? std::bit_cast<float>(regs.get(reg)) : disabled;
    };
    const float xscale = get(0, amd::mmPA_CL_VPORT_XSCALE, 1),
                xoffset = get(1, amd::mmPA_CL_VPORT_XOFFSET, 0),
                yscale = get(2, amd::mmPA_CL_VPORT_YSCALE, 1),
                yoffset = get(3, amd::mmPA_CL_VPORT_YOFFSET, 0),
                zscale = get(4, amd::mmPA_CL_VPORT_ZSCALE, 1),
                zoffset = get(5, amd::mmPA_CL_VPORT_ZOFFSET, 0);
    if (xscale == 0 || yscale == 0) {
        return std::nullopt;
    }
    const float min_depth = negative_one_to_one ? zoffset - zscale : zoffset;
    return VkViewport{xoffset - xscale,
                      yoffset - yscale,
                      2 * xscale,
                      2 * yscale,
                      std::clamp(min_depth, 0.0f, 1.0f),
                      std::clamp(zoffset + zscale, 0.0f, 1.0f)};
}

std::optional<DrawState> draw_state(const pm4::RegisterMirror& regs, bool quad_control,
                                    std::string& why) {
    DrawState s;
    PipelineKey& k = s.key;
    if (const std::uint32_t stages = regs.get(amd::mmVGT_SHADER_STAGES_EN); stages != 0) {
        return fail(why, std::format("VGT_SHADER_STAGES_EN {:#x}: only VS and PS are supported yet",
                                     stages));
    }
    k.vs = {regs.program_address(pm4::Stage::vs), 0, vertex_key(regs)};
    if (field(k.vs.key.pgm_rsrc2, 1, 5) >= 2) {  // USER_SGPR
        const auto user = regs.user_data(pm4::Stage::vs);
        k.vs.fetch_address = user[0] | std::uint64_t{user[1]} << 32;
    }
    if (const std::uint64_t ps = regs.program_address(pm4::Stage::ps); ps != 0) {
        k.ps = StageState{ps, 0, pixel_key(regs, quad_control)};
    }

    const std::uint32_t prim = regs.get(amd::mmVGT_PRIMITIVE_TYPE__CI__VI);
    const auto top = topology(prim);
    if (!top) {
        return fail(why, std::format("VGT_PRIMITIVE_TYPE {} is not supported yet", prim));
    }
    k.topology = *top;

    // PA_CL_CLIP_CNTL: CLIP_DISABLE (16), DX_CLIP_SPACE_DEF (19: z from 0 to w), ZCLIP_NEAR_DISABLE
    // (26) and ZCLIP_FAR_DISABLE (27). The clip space decides only what is clipped, so without
    // depth clipping z from 0 to w keeps the hardware's depth within 0-1.
    const std::uint32_t clip = regs.get(amd::mmPA_CL_CLIP_CNTL);
    k.depth_clamp = field(clip, 16, 1) != 0 || field(clip, 26, 2) != 0;
    k.negative_one_to_one = field(clip, 19, 1) == 0 && !k.depth_clamp;

    // PA_CL_VTE_CNTL: VTX_W0_FMT (10) without VTX_XY_FMT (8) and VTX_Z_FMT (9) divides by w.
    if (const std::uint32_t vte = regs.get(amd::mmPA_CL_VTE_CNTL); field(vte, 8, 3) != 4) {
        return fail(why, std::format("PA_CL_VTE_CNTL {:#x} is not supported yet", vte));
    }
    const auto vp = viewport(regs, k.negative_one_to_one);
    if (!vp) {
        return fail(why, "viewport 0 has a zero scale");
    }
    s.dynamic.viewport = *vp;
    return s;
}

}  // namespace bb::vk
