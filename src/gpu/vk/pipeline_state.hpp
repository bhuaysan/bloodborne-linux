// The draw snapshot: the pipeline state of a draw as the register mirror describes it, split into
// the key a pipeline is made and cached by and the state a draw sets dynamically
// (docs/design/graphics.md, "Pipeline state (item 26, as built)").
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <vulkan/vulkan.h>

#include "gpu/gcn/translate.hpp"
#include "gpu/pm4/registers.hpp"

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

// What a graphics pipeline is made from besides the targets' formats (RenderingInfo) and the
// translated stages: the state that Vulkan 1.3 can't set dynamically, plus the topology.
struct PipelineKey {
    StageState vs;
    std::optional<StageState> ps;  // none when SPI_SHADER_PGM_LO_PS is 0 (depth only)
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    // No depth clipping (CLIP_DISABLE or a ZCLIP_*_DISABLE of PA_CL_CLIP_CNTL): depthClampEnable.
    bool depth_clamp = false;
    // Clip-space z from -w to w (DX_CLIP_SPACE_DEF clear, with depth clipping):
    // VkPipelineViewportDepthClipControlCreateInfoEXT::negativeOneToOne.
    bool negative_one_to_one = false;
    bool operator==(const PipelineKey&) const = default;
};

// The state a draw sets on the command buffer (core dynamic state in Vulkan 1.3).
struct DynamicState {
    VkViewport viewport{};
};

struct DrawState {
    PipelineKey key;
    DynamicState dynamic;
};

// The snapshot of a draw. Nothing, with `why` saying what, when the state uses something not
// supported yet or viewport 0 has a zero scale (which draws nothing). `quad_control` is the
// device flag of the pixel key.
std::optional<DrawState> draw_state(const pm4::RegisterMirror& regs, bool quad_control,
                                    std::string& why);

// VGT_PRIMITIVE_TYPE (shadPS4's table); nothing for the types without an equivalent yet.
std::optional<VkPrimitiveTopology> topology(std::uint32_t prim_type);

// Viewport 0 of PA_CL_VPORT_*_0 and PA_CL_VTE_CNTL with the hardware's transform (a negative
// yscale gives a negative height; depth from zoffset, or zoffset - zscale with
// `negative_one_to_one`, to zoffset + zscale, clamped to 0-1). Nothing when xscale or yscale is 0.
std::optional<VkViewport> viewport(const pm4::RegisterMirror& regs, bool negative_one_to_one);

}  // namespace bb::vk
