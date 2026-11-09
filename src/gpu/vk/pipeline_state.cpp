// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/video_core/renderer_vulkan/liverpool_to_vk.cpp @0d5f2739d2544d2a35c73e354cba7a32b7a81bea
// Source: shadPS4 src/video_core/renderer_vulkan/vk_rasterizer.cpp @0d5f2739d2544d2a35c73e354cba7a32b7a81bea
// Source: shadPS4 src/video_core/amdgpu/regs_primitive.h @0d5f2739d2544d2a35c73e354cba7a32b7a81bea
// Source: shadPS4 src/video_core/amdgpu/regs_color.h @0d5f2739d2544d2a35c73e354cba7a32b7a81bea
// Changed for bloodborne-linux, 2026-11-07: the topology table without fallbacks; the viewport of
// UpdateViewportScissorState for viewport 0 of our register mirror, also when clipping is off.
// Changed for bloodborne-linux, 2026-11-09: the blend factor and function tables as numbers to
// optionals, without fallbacks; the raster and blend state in the pipeline key from our register
// mirror.

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

// PA_SU_SC_MODE_CNTL: POLY_MODE (bits 3-4, 1 enables, 2 and 3 reserved) with
// POLYMODE_FRONT_PTYPE (bits 5-7) and _BACK_PTYPE (bits 8-10): 0 points, 1 lines, 2 triangles.
// The polygon mode, or nothing when the types differ.
std::optional<VkPolygonMode> polygon_mode(std::uint32_t mode) {
    const std::uint32_t poly_mode = field(mode, 3, 2);
    if (poly_mode > 1) {
        return std::nullopt;  // 2 and 3 are reserved
    }
    if (poly_mode == 0) {
        return VK_POLYGON_MODE_FILL;
    }
    const std::uint32_t front = field(mode, 5, 3), back = field(mode, 8, 3);
    if (front != back || front > 2) {
        return std::nullopt;
    }
    constexpr VkPolygonMode modes[] = {VK_POLYGON_MODE_POINT, VK_POLYGON_MODE_LINE,
                                       VK_POLYGON_MODE_FILL};
    return modes[front];
}

// CB_BLEND<n>_CONTROL: COLOR_SRCBLEND (0-4), COLOR_COMB_FCN (5-7), COLOR_DESTBLEND (8-12),
// ALPHA_SRCBLEND (16-20), ALPHA_COMB_FCN (21-23), ALPHA_DESTBLEND (24-28), SEPARATE_ALPHA_BLEND
// (29), ENABLE (30). Without SEPARATE_ALPHA_BLEND the colour setting also blends alpha.
bool dual_source(std::uint32_t factor) { return factor >= 15 && factor <= 18; }

std::optional<BlendAttachment> blend_attachment(std::uint32_t control, std::uint32_t mask,
                                                bool bypass, std::string& why) {
    BlendAttachment a{.write_mask = static_cast<std::uint8_t>(mask)};
    // A target whose CB_TARGET_MASK nibble is 0 is not bound (the rendering setup skips it), so
    // its stale blend register is not decoded and cannot reject the draw or split the key.
    if (mask == 0 || field(control, 30, 1) == 0 || bypass) {
        return a;
    }
    const bool separate = field(control, 29, 1) != 0;
    const std::uint32_t color[] = {field(control, 0, 5), field(control, 5, 3), field(control, 8, 5)};
    const std::uint32_t alpha[] = {separate ? field(control, 16, 5) : color[0],
                                   separate ? field(control, 21, 3) : color[1],
                                   separate ? field(control, 24, 5) : color[2]};
    for (const std::uint32_t factor : {color[0], color[2], alpha[0], alpha[2]}) {
        if (dual_source(factor)) {
            why = std::format("dual-source blend factor {}", factor);
            return std::nullopt;
        }
    }
    const auto sc = blend_factor(color[0]), dc = blend_factor(color[2]);
    const auto sa = blend_factor(alpha[0]), da = blend_factor(alpha[2]);
    const auto oc = blend_op(color[1]), oa = blend_op(alpha[1]);
    if (!sc || !dc || !sa || !da || !oc || !oa) {
        why = std::format("CB_BLEND_CONTROL {:#x} is not supported", control);
        return std::nullopt;
    }
    a.enable = true;
    a.src_color = *sc, a.dst_color = *dc, a.src_alpha = *sa, a.dst_alpha = *da;
    a.color_op = *oc, a.alpha_op = *oa;
    return a;
}

bool raster_state(const pm4::RegisterMirror& regs, DrawState& s, std::string& why) {
    // PA_SU_SC_MODE_CNTL: CULL_FRONT (bit 0) and CULL_BACK (bit 1) are VkCullModeFlagBits, FACE
    // (bit 2) set is clockwise. The game's builders write these bits (tests/game).
    const std::uint32_t mode = regs.get(amd::mmPA_SU_SC_MODE_CNTL);
    const auto polygon = polygon_mode(mode);
    if (!polygon) {
        why = std::format(
            "PA_SU_SC_MODE_CNTL {:#x}: polygon types differ or are not supported, or POLY_MODE "
            "is reserved",
            mode);
        return false;
    }
    s.key.polygon_mode = *polygon;
    s.dynamic.cull_mode = field(mode, 0, 2);
    s.dynamic.front_face = field(mode, 2, 1) != 0 ? VK_FRONT_FACE_CLOCKWISE
                                                  : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    return true;
}

bool blend_state(const pm4::RegisterMirror& regs, DrawState& s, std::string& why) {
    constexpr std::uint32_t color_stride = amd::mmCB_COLOR1_INFO - amd::mmCB_COLOR0_INFO;
    const std::uint32_t target_mask = regs.get(amd::mmCB_TARGET_MASK);
    for (unsigned n = 0; n < s.key.blend.size(); ++n) {
        const std::uint32_t info = regs.get(amd::mmCB_COLOR0_INFO + n * color_stride);
        // INFO.BLEND_BYPASS (bit 16) skips the blender, as a disabled blend does.
        const auto a = blend_attachment(regs.get(amd::mmCB_BLEND0_CONTROL + n),
                                        field(target_mask, n * 4, 4), field(info, 16, 1) != 0, why);
        if (!a) {
            why = std::format("target {}: {}", n, why);
            return false;
        }
        s.key.blend[n] = *a;
    }
    for (unsigned c = 0; c < 4; ++c) {
        s.dynamic.blend_constants[c] = std::bit_cast<float>(regs.get(amd::mmCB_BLEND_RED + c));
    }
    return true;
}

}  // namespace

std::optional<VkBlendFactor> blend_factor(std::uint32_t factor) {
    switch (factor) {
    case 0: return VK_BLEND_FACTOR_ZERO;
    case 1: return VK_BLEND_FACTOR_ONE;
    case 2: return VK_BLEND_FACTOR_SRC_COLOR;
    case 3: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 6: return VK_BLEND_FACTOR_DST_ALPHA;
    case 7: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 8: return VK_BLEND_FACTOR_DST_COLOR;
    case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 13: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 14: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 15: return VK_BLEND_FACTOR_SRC1_COLOR;
    case 16: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case 17: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case 18: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case 19: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 20: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return std::nullopt;  // 11 and 12 are not defined, and nothing above 20
    }
}

std::optional<VkBlendOp> blend_op(std::uint32_t function) {
    switch (function) {
    case 0: return VK_BLEND_OP_ADD;
    case 1: return VK_BLEND_OP_SUBTRACT;
    case 2: return VK_BLEND_OP_MIN;
    case 3: return VK_BLEND_OP_MAX;
    case 4: return VK_BLEND_OP_REVERSE_SUBTRACT;
    default: return std::nullopt;
    }
}

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

std::optional<ComputeState> compute_state(const pm4::RegisterMirror& regs, std::string& why) {
    ComputeState s;
    s.code_address = regs.program_address(pm4::Stage::cs);
    if (s.code_address == 0) {
        why = "COMPUTE_PGM_LO/HI is 0";
        return std::nullopt;
    }
    s.key = {.stage = gcn::Stage::compute,
             .pgm_rsrc1 = regs.rsrc1(pm4::Stage::cs),
             .pgm_rsrc2 = regs.rsrc2(pm4::Stage::cs),
             .num_threads = {regs.num_threads(0), regs.num_threads(1), regs.num_threads(2)},
             .push_offset = 0};
    return s;
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
    if (!raster_state(regs, s, why) || !blend_state(regs, s, why)) {
        return std::nullopt;
    }
    return s;
}

}  // namespace bb::vk
