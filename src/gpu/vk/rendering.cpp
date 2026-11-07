#include "gpu/vk/rendering.hpp"

#include <algorithm>
#include <limits>
#include <span>
#include <vector>

#include "common/log.hpp"
#include "gpu/vk/device.hpp"
#include "gpu/vk/formats.hpp"
#include "gpu/vk/image_cache.hpp"
#include "si_ci_vi_merged_offset.h"

namespace bb::vk {

namespace {

namespace amd = Pal::Gfx6::Chip;

constexpr std::uint32_t field(std::uint32_t value, unsigned first, unsigned bits) {
    return value >> first & ((1u << bits) - 1);
}

constexpr std::uint32_t mode_normal = 1;  // CB_COLOR_CONTROL.MODE (bits 4-6)

// Moves the whole image to `layout`, after all earlier commands' writes and before `stages`.
VkImageMemoryBarrier2 transition(Image& image, VkImageLayout layout, VkPipelineStageFlags2 stages,
                                 VkAccessFlags2 access) {
    auto b = make<VkImageMemoryBarrier2>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2);
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = stages;
    b.dstAccessMask = access;
    b.oldLayout = image.layout;
    b.newLayout = layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image.handle;
    b.subresourceRange = {image.aspects, 0, 1, 0, image.key.slices};
    image.layout = layout;
    return b;
}

void barrier(VkCommandBuffer commands, std::span<const VkImageMemoryBarrier2> barriers) {
    auto dependency = make<VkDependencyInfo>(VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
    dependency.imageMemoryBarrierCount = static_cast<std::uint32_t>(barriers.size());
    dependency.pImageMemoryBarriers = barriers.data();
    vkCmdPipelineBarrier2(commands, &dependency);
}

VkRenderingAttachmentInfo attachment(VkImageView view, VkImageLayout layout) {
    auto a = make<VkRenderingAttachmentInfo>(VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO);
    a.imageView = view;
    a.imageLayout = layout;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    return a;
}

}  // namespace

TargetRenderer::~TargetRenderer() {
    for (const auto& [key, view] : views_) {
        vkDestroyImageView(device_.handle(), view, nullptr);
    }
}

VkImageView TargetRenderer::view(const Image& image, std::uint32_t base_slice,
                                 std::uint32_t layers) {
    const auto key = std::tuple{image.handle, base_slice, layers};
    if (const auto it = views_.find(key); it != views_.end()) {
        return it->second;
    }
    auto info = make<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    info.image = image.handle;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    info.format = image.format;
    info.subresourceRange = {image.aspects, 0, 1, base_slice, layers};
    VkImageView view = VK_NULL_HANDLE;
    check(vkCreateImageView(device_.handle(), &info, nullptr, &view), "vkCreateImageView");
    views_.emplace(key, view);
    return view;
}

std::optional<RenderingInfo> TargetRenderer::begin(VkCommandBuffer commands,
                                                   const pm4::RegisterMirror& regs,
                                                   const pm4::ExtentHints& hints) {
    constexpr auto most = std::numeric_limits<std::uint32_t>::max();
    RenderingInfo info;
    info.extent = {most, most};
    info.layers = most;
    bool any = false;
    std::vector<VkImageMemoryBarrier2> barriers;
    // The view of `image` for the slices base_slice to slices - 1 in `layout`, or none when the
    // view is empty or the sample count differs from the first attachment's.
    const auto bind = [&](Image& image, std::uint32_t base_slice, std::uint32_t slices,
                          VkImageLayout layout, VkPipelineStageFlags2 stages,
                          VkAccessFlags2 access) -> VkImageView {
        if (base_slice >= slices) {
            return VK_NULL_HANDLE;
        }
        if (any && image.key.samples != info.samples) {
            common::log(common::LogLevel::warn, "gpu.vk",
                        "target at {:#x} left out: {} samples, the rendering has {}",
                        image.key.address, image.key.samples, info.samples);
            return VK_NULL_HANDLE;
        }
        any = true;
        info.samples = image.key.samples;
        info.extent.width = std::min(info.extent.width, image.key.extent.width);
        info.extent.height = std::min(info.extent.height, image.key.extent.height);
        info.layers = std::min(info.layers, slices - base_slice);
        if (std::none_of(barriers.begin(), barriers.end(),
                         [&](const auto& b) { return b.image == image.handle; })) {
            barriers.push_back(transition(image, layout, stages, access));
        }
        return view(image, base_slice, slices - base_slice);
    };

    std::array<VkRenderingAttachmentInfo, pm4::color_targets> colors;
    colors.fill(attachment(VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED));
    const std::uint32_t target_mask = regs.get(amd::mmCB_TARGET_MASK);
    const bool normal = field(regs.get(amd::mmCB_COLOR_CONTROL), 4, 3) == mode_normal;
    for (unsigned slot = 0; normal && slot < pm4::color_targets; ++slot) {
        if (field(target_mask, slot * 4, 4) == 0) {
            continue;
        }
        const auto target = pm4::color_target(regs, hints, slot);
        const auto key = target ? color_image_key(*target) : std::nullopt;
        Image* image = key ? images_.get(*key) : nullptr;
        if (!image) {
            continue;
        }
        constexpr VkImageLayout layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        const VkImageView v =
            bind(*image, target->base_slice, target->slices, layout,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        if (!v) {
            continue;
        }
        VkRenderingAttachmentInfo& a = colors[slot] = attachment(v, layout);
        if (take_cleared(target->cmask_address)) {
            a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            a.clearValue.color = color_clear_value(image->format, target->clear_word);
        }
        info.color[slot] = image->format;
        info.color_count = slot + 1;
    }

    if (!any) {
        return std::nullopt;
    }
    barrier(commands, barriers);
    auto rendering = make<VkRenderingInfo>(VK_STRUCTURE_TYPE_RENDERING_INFO);
    rendering.renderArea = {{0, 0}, info.extent};
    rendering.layerCount = info.layers;
    rendering.colorAttachmentCount = info.color_count;
    rendering.pColorAttachments = colors.data();
    vkCmdBeginRendering(commands, &rendering);
    return info;
}

}  // namespace bb::vk
