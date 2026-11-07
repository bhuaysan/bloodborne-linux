#include "gpu/vk/rendering.hpp"

#include <algorithm>
#include <cmath>
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

// CB_COLOR_CONTROL.MODE (bits 4-6).
constexpr std::uint32_t mode_normal = 1;
constexpr std::uint32_t mode_eliminate_fast_clear = 2;

std::uint32_t color_mode(const pm4::RegisterMirror& regs) {
    return field(regs.get(amd::mmCB_COLOR_CONTROL), 4, 3);
}

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

// A clear of one aspect of a view's slices, recorded before the rendering or by the eliminate
// pass.
struct Clear {
    Image* image;
    VkImageSubresourceRange range;
    VkClearValue value;
};

// Moves the cleared images to TRANSFER_DST_OPTIMAL and records the clears.
void record_clears(VkCommandBuffer commands, std::span<const Clear> clears) {
    std::vector<VkImageMemoryBarrier2> barriers;
    for (const Clear& c : clears) {
        if (std::none_of(barriers.begin(), barriers.end(),
                         [&](const auto& b) { return b.image == c.image->handle; })) {
            barriers.push_back(transition(*c.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                          VK_PIPELINE_STAGE_2_CLEAR_BIT,
                                          VK_ACCESS_2_TRANSFER_WRITE_BIT));
        }
    }
    barrier(commands, barriers);
    for (const Clear& c : clears) {
        if (c.range.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT) {
            vkCmdClearColorImage(commands, c.image->handle, c.image->layout, &c.value.color, 1,
                                 &c.range);
        } else {
            vkCmdClearDepthStencilImage(commands, c.image->handle, c.image->layout,
                                        &c.value.depthStencil, 1, &c.range);
        }
    }
}

// The clear of colour target `target`'s view to its clear words.
Clear color_clear(Image& image, const pm4::ColorTarget& target) {
    Clear c{&image,
            {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, target.base_slice, target.slices - target.base_slice},
            {}};
    c.value.color = color_clear_value(image.format, target.clear_word);
    return c;
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
    // Attachment usage only: the image is also sampled, but a sampled view of a depth-stencil
    // image needs a single aspect, and texture views are made elsewhere.
    auto usage = make<VkImageViewUsageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO);
    usage.usage = image.aspects == VK_IMAGE_ASPECT_COLOR_BIT
                      ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                      : VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    auto info = make<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, &usage);
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
    // The images bound, each with the layout, stages and access of its attachment.
    struct Use {
        Image* image;
        VkImageLayout layout;
        VkPipelineStageFlags2 stages;
        VkAccessFlags2 access;
    };
    std::vector<Use> uses;
    std::vector<Clear> clears;  // of marked metadata, over the whole view
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
        if (std::none_of(uses.begin(), uses.end(),
                         [&](const Use& u) { return u.image == &image; })) {
            uses.push_back({&image, layout, stages, access});
        }
        return view(image, base_slice, slices - base_slice);
    };

    std::array<VkRenderingAttachmentInfo, pm4::color_targets> colors;
    colors.fill(attachment(VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED));
    const std::uint32_t target_mask = regs.get(amd::mmCB_TARGET_MASK);
    const bool normal = color_mode(regs) == mode_normal;
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
        colors[slot] = attachment(v, layout);
        if (take_cleared(target->cmask_address)) {
            clears.push_back(color_clear(*image, *target));
        }
        info.color[slot] = image->format;
        info.color_count = slot + 1;
    }

    VkRenderingAttachmentInfo depth = attachment(VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED);
    VkRenderingAttachmentInfo stencil = depth;
    const std::uint32_t depth_control = regs.get(amd::mmDB_DEPTH_CONTROL);
    const bool stencil_test = field(depth_control, 0, 1) != 0;
    const bool depth_test = field(depth_control, 1, 1) != 0;
    const bool depth_write = field(depth_control, 2, 1) != 0;
    const auto target = pm4::depth_target(regs, hints);
    if (target && ((target->z_format != 0 && depth_test) || (target->stencil && stencil_test))) {
        const auto key = depth_image_key(*target);
        Image* image = key ? images_.get(*key) : nullptr;
        constexpr VkImageLayout layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        const VkImageView v =
            image ? bind(*image, target->base_slice, target->slices, layout,
                         VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                             VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                             VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)
                  : VK_NULL_HANDLE;
        if (v) {
            depth = stencil = attachment(v, layout);
            if (target->z_format != 0) {
                info.depth = image->format;
                VkClearValue value{};
                // fmax and fmin also turn a NaN into 0.
                value.depthStencil.depth = std::fmin(std::fmax(target->depth_clear, 0.0f), 1.0f);
                if (target->clear_depth && depth_test && depth_write) {
                    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                    depth.clearValue = value;
                }
                if (target->htile_address != 0 && take_cleared(target->htile_address)) {
                    clears.push_back({image,
                                      {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, target->base_slice,
                                       target->slices - target->base_slice},
                                      value});
                }
            }
            if (target->stencil) {
                info.stencil = image->format;
                if (target->clear_stencil) {
                    stencil.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                    stencil.clearValue.depthStencil.stencil = target->stencil_clear;
                }
            }
        }
    }

    if (!any) {
        return std::nullopt;
    }
    if (!clears.empty()) {
        record_clears(commands, clears);
    }
    std::vector<VkImageMemoryBarrier2> barriers;
    for (const Use& u : uses) {
        barriers.push_back(transition(*u.image, u.layout, u.stages, u.access));
    }
    barrier(commands, barriers);
    auto rendering = make<VkRenderingInfo>(VK_STRUCTURE_TYPE_RENDERING_INFO);
    rendering.renderArea = {{0, 0}, info.extent};
    rendering.layerCount = info.layers;
    rendering.colorAttachmentCount = info.color_count;
    rendering.pColorAttachments = colors.data();
    rendering.pDepthAttachment = info.depth != VK_FORMAT_UNDEFINED ? &depth : nullptr;
    rendering.pStencilAttachment = info.stencil != VK_FORMAT_UNDEFINED ? &stencil : nullptr;
    vkCmdBeginRendering(commands, &rendering);
    return info;
}

bool TargetRenderer::eliminate_fast_clear(VkCommandBuffer commands,
                                          const pm4::RegisterMirror& regs,
                                          const pm4::ExtentHints& hints) {
    if (color_mode(regs) != mode_eliminate_fast_clear) {
        return false;
    }
    const auto target = pm4::color_target(regs, hints, 0);
    if (!target || !target->fast_clear || target->base_slice >= target->slices ||
        !cleared(target->cmask_address)) {
        return false;
    }
    const auto key = color_image_key(*target);
    Image* image = key ? images_.get(*key) : nullptr;
    if (!image) {
        return false;
    }
    take_cleared(target->cmask_address);
    const Clear clear = color_clear(*image, *target);
    record_clears(commands, {&clear, 1});
    return true;
}

}  // namespace bb::vk
