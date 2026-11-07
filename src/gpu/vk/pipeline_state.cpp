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

// Viewport 0: PA_CL_VPORT_*_0, with PA_CL_VTE_CNTL bits 0-5 enabling the x scale and offset,
// then y, then z. A disabled scale is 1, a disabled offset 0.
struct Transform {
    float xscale, xoffset, yscale, yoffset, zscale, zoffset;
};

Transform transform(const pm4::RegisterMirror& regs) {
    const std::uint32_t vte = regs.get(amd::mmPA_CL_VTE_CNTL);
    const auto get = [&](unsigned bit, std::uint32_t reg, float disabled) {
        return field(vte, bit, 1) != 0 ? std::bit_cast<float>(regs.get(reg)) : disabled;
    };
    return {get(0, amd::mmPA_CL_VPORT_XSCALE, 1), get(1, amd::mmPA_CL_VPORT_XOFFSET, 0),
            get(2, amd::mmPA_CL_VPORT_YSCALE, 1), get(3, amd::mmPA_CL_VPORT_YOFFSET, 0),
            get(4, amd::mmPA_CL_VPORT_ZSCALE, 1), get(5, amd::mmPA_CL_VPORT_ZOFFSET, 0)};
}

// The hardware's transform: a negative yscale gives a negative height; depth from zoffset, or
// zoffset - zscale with `negative_one_to_one`, to zoffset + zscale, clamped to 0-1.
VkViewport viewport(const Transform& t, bool negative_one_to_one) {
    const float min_depth = negative_one_to_one ? t.zoffset - t.zscale : t.zoffset;
    return {t.xoffset - t.xscale,
            t.yoffset - t.yscale,
            2 * t.xscale,
            2 * t.yscale,
            std::clamp(min_depth, 0.0f, 1.0f),
            std::clamp(t.zoffset + t.zscale, 0.0f, 1.0f)};
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

    // PA_CL_VTE_CNTL: VTX_W0_FMT (10) without VTX_XY_FMT (8) and VTX_Z_FMT (9) divides by w.
    if (const std::uint32_t vte = regs.get(amd::mmPA_CL_VTE_CNTL); field(vte, 8, 3) != 4) {
        return fail(why, std::format("PA_CL_VTE_CNTL {:#x} is not supported yet", vte));
    }
    const Transform t = transform(regs);
    if (t.xscale == 0 || t.yscale == 0) {
        return fail(why, "viewport 0 has a zero scale");
    }
    if (t.xscale < 0) {  // a Vulkan viewport may only have a negative height
        return fail(why, "viewport 0 has a negative x scale");
    }

    // PA_CL_CLIP_CNTL: CLIP_DISABLE (16), DX_CLIP_SPACE_DEF (19: z from 0 to w), ZCLIP_NEAR_DISABLE
    // (26) and ZCLIP_FAR_DISABLE (27). The clip space decides only what is clipped; the transform
    // is the same in both conventions. Without depth clipping only the clamp range differs (in
    // Vulkan the viewport's depth range, on the hardware PA_SC_VPORT_ZMIN/ZMAX, normally 0-1), so
    // z goes from -w to w when the whole range zoffset +- zscale lies within 0-1, and from 0 to w
    // (clamped to zoffset..zoffset + zscale) otherwise.
    const std::uint32_t clip = regs.get(amd::mmPA_CL_CLIP_CNTL);
    k.depth_clamp = field(clip, 16, 1) != 0 || field(clip, 26, 2) != 0;
    if (k.depth_clamp) {
        const float low = t.zoffset - t.zscale, high = t.zoffset + t.zscale;
        k.negative_one_to_one = std::min(low, high) >= 0 && std::max(low, high) <= 1;
    } else {
        k.negative_one_to_one = field(clip, 19, 1) == 0;
    }
    s.dynamic.viewport = viewport(t, k.negative_one_to_one);
    return s;
}

}  // namespace bb::vk
