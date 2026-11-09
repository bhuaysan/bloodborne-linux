#include "gpu/vk/draw_recorder.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <exception>
#include <format>
#include <optional>
#include <stdexcept>
#include <tuple>

#include "common/log.hpp"
#include "si_ci_vi_merged_offset.h"
#include "gpu/gcn/translate.hpp"
#include "gpu/vk/device.hpp"
#include "gpu/vk/dispatcher.hpp"
#include "gpu/vk/memory_mirror.hpp"
#include "gpu/vk/pipeline_cache.hpp"
#include "gpu/vk/rendering.hpp"
#include "gpu/vk/scheduler.hpp"
#include "gpu/vk/texture_binder.hpp"

namespace amd = Pal::Gfx6::Chip;

namespace bb::vk {
namespace {

constexpr const char* channel = "gpu.vk";

// Stops a rendering that begin() started when recording the draw throws.
struct Rendering {
    VkCommandBuffer commands;
    bool done = false;
    ~Rendering() {
        if (!done) {
            TargetRenderer::end(commands);
        }
    }
};

void set_dynamic_state(VkCommandBuffer commands, const DynamicState& d, VkExtent2D extent) {
    vkCmdSetViewport(commands, 0, 1, &d.viewport);
    // The scissor stays inside the render area.
    VkRect2D scissor = d.scissor;
    scissor.offset.x = std::min<std::int32_t>(scissor.offset.x, extent.width);
    scissor.offset.y = std::min<std::int32_t>(scissor.offset.y, extent.height);
    scissor.extent.width = std::min(scissor.extent.width, extent.width - scissor.offset.x);
    scissor.extent.height = std::min(scissor.extent.height, extent.height - scissor.offset.y);
    vkCmdSetScissor(commands, 0, 1, &scissor);
    vkCmdSetCullMode(commands, d.cull_mode);
    vkCmdSetFrontFace(commands, d.front_face);
    vkCmdSetDepthTestEnable(commands, d.depth_test);
    vkCmdSetDepthWriteEnable(commands, d.depth_write);
    vkCmdSetDepthCompareOp(commands, d.depth_compare);
    vkCmdSetStencilTestEnable(commands, d.stencil_test);
    const std::pair<VkStencilFaceFlags, const StencilFace*> faces[] = {
        {VK_STENCIL_FACE_FRONT_BIT, &d.front}, {VK_STENCIL_FACE_BACK_BIT, &d.back}};
    for (const auto& [face, s] : faces) {
        vkCmdSetStencilOp(commands, face, s->fail, s->pass, s->depth_fail, s->compare);
        vkCmdSetStencilCompareMask(commands, face, s->compare_mask);
        vkCmdSetStencilWriteMask(commands, face, s->write_mask);
        vkCmdSetStencilReference(commands, face, s->reference);
    }
    vkCmdSetDepthBiasEnable(commands, d.depth_bias);
    vkCmdSetDepthBias(commands, d.bias_constant, 0, d.bias_slope);
    vkCmdSetBlendConstants(commands, d.blend_constants.data());
    vkCmdSetPrimitiveRestartEnable(commands, d.primitive_restart);
}

// Sets `key.images` to the classes of the T#s that `interface` reads now (empty when all are 2D
// float, the key's default). Returns whether that changed the key, or nothing, with `why`, when a
// T# can't be read.
std::optional<bool> set_classes(TextureBinder& binder, const gcn::ShaderInterface& interface,
                                std::span<const std::uint32_t, pm4::user_data_dwords> user,
                                gcn::ShaderKey& key, std::string& why) {
    auto classes = binder.classes(interface, user, why);
    if (!classes) {
        return std::nullopt;
    }
    if (std::ranges::all_of(*classes, [](const gcn::ImageClass& c) { return c == gcn::ImageClass{}; })) {
        classes->clear();
    }
    const bool changed = key.images != *classes;
    key.images = std::move(*classes);
    return changed;
}

}  // namespace

void DrawRecorder::draw(const pm4::RegisterMirror& regs, const pm4::DrawCall& call) {
    std::string why;
    try {
        if (record(regs, call, why)) {
            ++recorded_;
            return;
        }
        ++unsupported_;
        if (logged_.insert(why).second) {
            common::log(common::LogLevel::warn, channel, "draw skipped: {}", why);
        }
    } catch (const std::exception& e) {
        const std::uint64_t n = failed_.fetch_add(1) + 1;
        // A lost device fails every draw: log the first 16, then only at powers of two.
        if (n <= 16 || std::has_single_bit(n)) {
            common::log(common::LogLevel::error, channel, "draw failed ({}): {}", n, e.what());
        }
        dispatcher_.report(e);
    }
}

bool DrawRecorder::record(const pm4::RegisterMirror& regs, const pm4::DrawCall& call,
                          std::string& why) {
    if (call.indirect != 0) {
        why = "indirect draws are not supported yet";
        return false;
    }
    const auto state = draw_state(regs, device_.quad_control(), why);
    if (!state) {
        return false;
    }

    // Vulkan restarts only on the all-ones value of the index type. The snapshot accepts either
    // all-ones value, the draw knows which one is the right one.
    const bool indexed = call.kind != pm4::DrawKind::index_auto;
    if (indexed && state->dynamic.primitive_restart) {
        const std::uint32_t all_ones = call.index_bytes == 4 ? 0xffffffffu : 0xffffu;
        if (regs.get(amd::mmVGT_MULTI_PRIM_IB_RESET_INDX) != all_ones) {
            why = "primitive restart with a reset index that is not all ones for the index type";
            return false;
        }
    }

    // The index buffer lives in the mirror: buffer k at the window offsets of its 2 GiB.
    VkBuffer indices = VK_NULL_HANDLE;
    std::uint64_t indices_offset = 0;
    if (indexed) {
        const std::uint64_t window = call.index_address - mirror_.window_start();
        indices_offset = window % mirror_buffer_size;
        if (call.index_address < mirror_.window_start() ||
            window / mirror_buffer_size >= mirror_.size() / mirror_buffer_size ||
            indices_offset % call.index_bytes != 0) {
            throw std::runtime_error(std::format(
                "index buffer at {:#x} is outside the mirror or unaligned", call.index_address));
        }
        if (call.max_indices != 0 &&
            std::uint64_t{call.first_index} + call.index_count > call.max_indices) {
            throw std::runtime_error(std::format(
                "indices at {:#x} ({} from {}) run past the {} the packet allows",
                call.index_address, call.index_count, call.first_index, call.max_indices));
        }
        // The device has no robust buffer access: the whole index range must lie in the buffer.
        const std::uint64_t end =
            indices_offset + (std::uint64_t{call.first_index} + call.index_count) * call.index_bytes;
        if (end > mirror_buffer_size) {
            throw std::runtime_error(std::format(
                "indices at {:#x} ({} from {}) run past the end of the mirror buffer",
                call.index_address, call.index_count, call.first_index));
        }
        indices = mirror_.buffer(window / mirror_buffer_size);
    }

    const VkCommandBuffer commands = scheduler_.commands();
    // Order the draw after earlier dispatches and draws that wrote what it reads.
    auto barrier = make<VkMemoryBarrier2>(VK_STRUCTURE_TYPE_MEMORY_BARRIER_2);
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                           VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                           VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT |
                            VK_ACCESS_2_INDEX_READ_BIT;
    auto dependency = make<VkDependencyInfo>(VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(commands, &dependency);

    // One rendering per draw: a dispatch between two draws must not sit inside one.
    const auto targets = targets_.begin(commands, regs, hints_);
    if (!targets) {
        why = "no render target is bound";
        return false;
    }
    Rendering rendering{commands};
    const GraphicsPipeline* pipeline = pipelines_.get(state->key, *targets, why);
    if (pipeline == nullptr) {
        return false;
    }
    // The textures: a pipeline translated for other image classes than the T#s have is replaced,
    // then their descriptor sets are made. That uploads them, which a rendering can't contain, so
    // it is ended and begun again.
    std::array<VkDescriptorSet, 2> sets{};  // set 0 for the VS, set 1 for the PS
    if (!pipeline->vs.images.empty() || !pipeline->ps.images.empty()) {
        if (textures_ == nullptr) {
            why = "the shaders sample images, and there are no textures to bind";
            return false;
        }
        PipelineKey keyed = state->key;
        const auto vs_user = regs.user_data(pm4::Stage::vs);
        const auto ps_user = regs.user_data(pm4::Stage::ps);
        const auto vs_changed = set_classes(*textures_, pipeline->vs, vs_user, keyed.vs.key, why);
        const auto ps_changed = keyed.ps ? set_classes(*textures_, pipeline->ps, ps_user,
                                                       keyed.ps->key, why)
                                         : std::optional<bool>(false);
        if (!vs_changed || !ps_changed) {
            return false;
        }
        if ((*vs_changed || *ps_changed) &&
            (pipeline = pipelines_.get(keyed, *targets, why)) == nullptr) {
            return false;
        }
        TargetRenderer::end(commands);
        rendering.done = true;
        for (const auto& [set, interface, user] :
             {std::tuple{0, &pipeline->vs, vs_user}, {1, &pipeline->ps, ps_user}}) {
            if (!interface->images.empty() &&
                (sets[set] = textures_->bind(*interface, user, why)) == VK_NULL_HANDLE) {
                return false;
            }
        }
        if (!targets_.begin(commands, regs, hints_)) {
            why = "no render target is bound";
            return false;
        }
        rendering.done = false;
    }
    std::uint64_t blocks[2] = {};  // the VS's and the PS's draw block, as the push constants
    blocks[0] = dispatcher_.draw_block(regs.user_data(pm4::Stage::vs), state->key.vs.code_address);
    if (state->key.ps) {
        blocks[1] = dispatcher_.draw_block(regs.user_data(pm4::Stage::ps),
                                           state->key.ps->code_address);
    }

    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline);
    static_assert(sizeof blocks == gcn::graphics_push_size);
    vkCmdPushConstants(commands, pipeline->layout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       gcn::graphics_push_size, blocks);
    for (std::uint32_t set = 0; set < sets.size(); ++set) {
        if (sets[set] != VK_NULL_HANDLE) {
            vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->layout,
                                    set, 1, &sets[set], 0, nullptr);
        }
    }
    set_dynamic_state(commands, state->dynamic, targets->extent);
    if (indexed) {
        vkCmdBindIndexBuffer(commands, indices, indices_offset,
                             call.index_bytes == 4 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(commands, call.index_count, call.instances, call.first_index, 0, 0);
    } else {
        vkCmdDraw(commands, call.index_count, call.instances, 0, 0);
    }
    rendering.done = true;
    TargetRenderer::end(commands);
    return true;
}

}  // namespace bb::vk
