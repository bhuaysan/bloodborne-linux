// The draw snapshot: the pipeline state of a draw as the register mirror describes it, split into
// the key a pipeline is made and cached by and the state a draw sets dynamically
// (docs/design/graphics.md, "Pipeline state (item 26, as built)").
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include <vulkan/vulkan.h>

#include "gpu/gcn/translate.hpp"
#include "gpu/pm4/registers.hpp"
#include "gpu/pm4/targets.hpp"

namespace bb::vk {

// One graphics stage: its program (read when it is translated), the fetch subroutine of the VS
// and its ShaderKey.
struct StageState {
    std::uint64_t code_address = 0;  // SPI_SHADER_PGM_LO/HI_<stage>
    // VS: user-data dwords 0-1 when USER_SGPR is at least 2, else 0. Only a program that calls a
    // fetch subroutine (s_swappc_b64) uses it, so the pipeline cache compares it only for those.
    std::uint64_t fetch_address = 0;
    gcn::ShaderKey key;
    bool operator==(const StageState&) const = default;
};

// The blend state of one colour target (CB_BLEND<n>_CONTROL with CB_TARGET_MASK). Blend attachment
// state isn't core dynamic state, so it is part of the key; a target that doesn't blend has the
// default factors, so equal states compare equal.
struct BlendAttachment {
    bool enable = false;
    std::uint8_t write_mask = 0xf;  // CB_TARGET_MASK nibble n: bit 0 red to bit 3 alpha
    VkBlendFactor src_color = VK_BLEND_FACTOR_ONE, dst_color = VK_BLEND_FACTOR_ZERO;
    VkBlendFactor src_alpha = VK_BLEND_FACTOR_ONE, dst_alpha = VK_BLEND_FACTOR_ZERO;
    VkBlendOp color_op = VK_BLEND_OP_ADD, alpha_op = VK_BLEND_OP_ADD;
    bool operator==(const BlendAttachment&) const = default;
};

// What a graphics pipeline is made from besides the targets' formats (RenderingInfo) and the
// translated stages: the state that Vulkan 1.3 can't set dynamically, plus the topology.
struct PipelineKey {
    StageState vs;
    std::optional<StageState> ps;  // none when SPI_SHADER_PGM_LO_PS is 0 (depth only)
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    // VGT_PRIMITIVE_TYPE 17: a triangle list of three corners per rectangle, which a geometry
    // stage completes with the fourth (gcn::rect_list_geometry).
    bool rect_list = false;
    // No depth clipping (CLIP_DISABLE or a ZCLIP_*_DISABLE of PA_CL_CLIP_CNTL): depthClampEnable.
    bool depth_clamp = false;
    // Clip-space z from -w to w: VkPipelineViewportDepthClipControlCreateInfoEXT::negativeOneToOne.
    // With depth clipping when DX_CLIP_SPACE_DEF is clear, without it when zoffset +- zscale lies
    // within 0-1 (the depth range then clamps as little as possible).
    bool negative_one_to_one = false;
    // PA_SU_SC_MODE_CNTL.POLY_MODE: fill, or line or point when both polygon types agree.
    VkPolygonMode polygon_mode = VK_POLYGON_MODE_FILL;
    std::array<BlendAttachment, pm4::color_targets> blend;  // by colour target slot
    bool operator==(const PipelineKey&) const = default;
};

// The compute state of a dispatch: the program and its ShaderKey, from the CS registers.
struct ComputeState {
    std::uint64_t code_address = 0;  // COMPUTE_PGM_LO/HI
    gcn::ShaderKey key;
    bool operator==(const ComputeState&) const = default;
};

// The stencil state of one face (DB_STENCIL_CONTROL, DB_STENCILREFMASK[_BF]).
struct StencilFace {
    VkStencilOp fail = VK_STENCIL_OP_KEEP, pass = VK_STENCIL_OP_KEEP,
                depth_fail = VK_STENCIL_OP_KEEP;
    VkCompareOp compare = VK_COMPARE_OP_ALWAYS;
    std::uint32_t compare_mask = 0, write_mask = 0, reference = 0;
    bool operator==(const StencilFace&) const = default;
};

// The state a draw sets on the command buffer (core dynamic state in Vulkan 1.3).
struct DynamicState {
    VkViewport viewport{};
    VkCullModeFlags cull_mode = VK_CULL_MODE_NONE;
    VkFrontFace front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    std::array<float, 4> blend_constants{};  // CB_BLEND_RED to CB_BLEND_ALPHA
    bool depth_test = false;
    bool depth_write = false;  // off under DB_RENDER_CONTROL.DEPTH_CLEAR_ENABLE
    VkCompareOp depth_compare = VK_COMPARE_OP_ALWAYS;
    bool stencil_test = false;
    StencilFace front, back;  // write masks 0 under STENCIL_CLEAR_ENABLE
    // The scissor of viewport 0: the screen, generic and window scissors and, with
    // VPORT_SCISSOR_ENABLE, the viewport's own, intersected (never negative; empty draws
    // nothing). The recording clips it to the render area.
    VkRect2D scissor{};
    // Depth bias from PA_SU_SC_MODE_CNTL.POLY_OFFSET_*_ENABLE (front's values when it is enabled,
    // else back's): the constant offset and the slope scale (the register / 16). The clamp
    // (PA_SU_POLY_OFFSET_CLAMP) is not applied: depthBiasClamp is not an enabled feature.
    bool depth_bias = false;
    float bias_constant = 0, bias_slope = 0;
    // VGT_MULTI_PRIM_IB_RESET_EN on a strip or fan topology (list restart needs a feature we
    // don't enable). The restart index is taken as the all-ones index of the draw's type.
    bool primitive_restart = false;
};

struct DrawState {
    PipelineKey key;
    DynamicState dynamic;
};

// The snapshot of a draw. Nothing, with `why` saying what, when the state uses something not
// supported yet, viewport 0 has a zero scale (which draws nothing) or a negative x scale (no valid
// Vulkan viewport). `quad_control` is the device flag of the pixel key.
std::optional<DrawState> draw_state(const pm4::RegisterMirror& regs, bool quad_control,
                                    std::string& why);

// The snapshot of a dispatch: the program address (COMPUTE_PGM_LO/HI) and the key from
// COMPUTE_PGM_RSRC1/2 and COMPUTE_NUM_THREAD_X/Y/Z (docs/design/graphics.md, item 25). The
// push offset is 0. Nothing for a zero program address, with `why` saying so.
std::optional<ComputeState> compute_state(const pm4::RegisterMirror& regs, std::string& why);

// The hardware's blend factor (CB_BLENDn_CONTROL's 5-bit fields), blend function and stencil
// operation (DB_STENCIL_CONTROL's nibbles) as Vulkan's; nothing for the values Vulkan lacks
// (unused factor numbers, the stencil operations 2 and 10-15). Dual-source factors (15-18) are
// returned too; `draw_state` rejects them. `compare_op` takes the 3-bit DB_DEPTH_CONTROL function.
std::optional<VkBlendFactor> blend_factor(std::uint32_t factor);
std::optional<VkBlendOp> blend_op(std::uint32_t function);
std::optional<VkStencilOp> stencil_op(std::uint32_t op);
VkCompareOp compare_op(std::uint32_t function);

// VGT_PRIMITIVE_TYPE of a rectangle list, which topology() does not map.
constexpr std::uint32_t rect_list_primitive = 17;
// VGT_PRIMITIVE_TYPE (shadPS4's table); nothing for the types without an equivalent yet.
std::optional<VkPrimitiveTopology> topology(std::uint32_t prim_type);

}  // namespace bb::vk
