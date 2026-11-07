#include "gpu/runner/graphics_runner.hpp"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

#include <vulkan/vulkan.h>

#include "gpu/vk/device.hpp"
#include "gpu/vk/scheduler.hpp"

namespace bb::test::vk {

namespace {

using bb::vk::check;
using bb::vk::make;

constexpr VkFormat color_format = VK_FORMAT_R32G32B32A32_SFLOAT;
constexpr std::size_t texel_size = 16;
constexpr VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

// The objects of one render. The scheduler keeps them until the render's work has completed.
struct Render {
    VkDevice device;
    Image target;
    std::vector<Image> textures;
    VkSampler sampler = VK_NULL_HANDLE;
    std::array<VkDescriptorSetLayout, 2> set_layouts{};
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    std::array<VkShaderModule, 2> modules{};
    VkPipeline pipeline = VK_NULL_HANDLE;

    ~Render() {
        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyPipelineLayout(device, layout, nullptr);
        for (VkDescriptorSetLayout set_layout : set_layouts) {
            vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
        }
        vkDestroyDescriptorPool(device, pool, nullptr);
        vkDestroySampler(device, sampler, nullptr);
        for (VkShaderModule module : modules) {
            vkDestroyShaderModule(device, module, nullptr);
        }
        const auto destroy = [this](const Image& i) {
            vkDestroyImageView(device, i.view, nullptr);
            vkDestroyImage(device, i.image, nullptr);
            vkFreeMemory(device, i.memory, nullptr);
        };
        std::ranges::for_each(textures, destroy);
        destroy(target);
    }
};

VkShaderModule create_module(VkDevice device, std::span<const std::uint32_t> spirv) {
    auto info = make<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
    info.codeSize = spirv.size_bytes();
    info.pCode = spirv.data();
    VkShaderModule module = VK_NULL_HANDLE;
    check(vkCreateShaderModule(device, &info, nullptr, &module), "vkCreateShaderModule");
    return module;
}

// An R32G32B32A32_SFLOAT image of `texture`'s size, levels and layers, and its view.
void create_image(const bb::vk::Device& device, VkDevice handle, Image& out,
                  const Texture& texture, VkImageUsageFlags usage) {
    auto info = make<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = color_format;
    info.extent = {texture.width, texture.height, 1};
    info.mipLevels = texture.levels;
    info.arrayLayers = texture.layers;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    check(vkCreateImage(handle, &info, nullptr, &out.image), "vkCreateImage");
    VkMemoryRequirements needs;
    vkGetImageMemoryRequirements(handle, out.image, &needs);
    auto alloc = make<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
    alloc.allocationSize = needs.size;
    alloc.memoryTypeIndex = device.memory_type(needs.memoryTypeBits, 0).value();
    check(vkAllocateMemory(handle, &alloc, nullptr, &out.memory), "vkAllocateMemory");
    check(vkBindImageMemory(handle, out.image, out.memory, 0), "vkBindImageMemory");
    auto view = make<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    view.image = out.image;
    view.viewType = texture.arrayed ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    view.format = color_format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, texture.levels, 0, texture.layers};
    check(vkCreateImageView(handle, &view, nullptr, &out.view), "vkCreateImageView");
}

// Descriptor set 0 empty, set 1 with `textures` sampled images and then `samplers` samplers for
// the fragment stage, and the push constant.
void create_layout(Render& r, std::size_t textures, std::uint32_t samplers) {
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for (std::uint32_t i = 0; i < textures + samplers; ++i) {
        bindings.push_back({i,
                            i < textures ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                         : VK_DESCRIPTOR_TYPE_SAMPLER,
                            1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
    }
    auto set = make<VkDescriptorSetLayoutCreateInfo>(
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
    check(vkCreateDescriptorSetLayout(r.device, &set, nullptr, &r.set_layouts[0]),
          "vkCreateDescriptorSetLayout");
    set.bindingCount = static_cast<std::uint32_t>(bindings.size());
    set.pBindings = bindings.data();
    check(vkCreateDescriptorSetLayout(r.device, &set, nullptr, &r.set_layouts[1]),
          "vkCreateDescriptorSetLayout");
    VkPushConstantRange range{stages, 0, sizeof(VkDeviceAddress)};
    auto info = make<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    info.setLayoutCount = 2;
    info.pSetLayouts = r.set_layouts.data();
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &range;
    check(vkCreatePipelineLayout(r.device, &info, nullptr, &r.layout), "vkCreatePipelineLayout");
}

// Set 1 with the textures and the sampler in every sampler binding.
VkDescriptorSet create_set(Render& r, std::uint32_t samplers) {
    auto sampler = make<VkSamplerCreateInfo>(VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
    sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.maxLod = VK_LOD_CLAMP_NONE;
    check(vkCreateSampler(r.device, &sampler, nullptr, &r.sampler), "vkCreateSampler");
    const auto textures = static_cast<std::uint32_t>(r.textures.size());
    const std::array<VkDescriptorPoolSize, 2> sizes = {
        {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, std::max(textures, 1u)},
         {VK_DESCRIPTOR_TYPE_SAMPLER, std::max(samplers, 1u)}}};
    auto pool = make<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
    pool.maxSets = 1;
    pool.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
    pool.pPoolSizes = sizes.data();
    check(vkCreateDescriptorPool(r.device, &pool, nullptr, &r.pool), "vkCreateDescriptorPool");
    auto alloc = make<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
    alloc.descriptorPool = r.pool;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &r.set_layouts[1];
    VkDescriptorSet set = VK_NULL_HANDLE;
    check(vkAllocateDescriptorSets(r.device, &alloc, &set), "vkAllocateDescriptorSets");
    std::vector<VkDescriptorImageInfo> images(textures + samplers);
    std::vector<VkWriteDescriptorSet> writes(images.size());
    for (std::uint32_t i = 0; i < images.size(); ++i) {
        const bool texture = i < textures;
        images[i] = {texture ? VK_NULL_HANDLE : r.sampler,
                     texture ? r.textures[i].view : VK_NULL_HANDLE,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        writes[i] = make<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType =
            texture ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER;
        writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(r.device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0,
                           nullptr);
    return set;
}

void create_pipeline(Render& r, const ColorTarget& target) {
    std::array<VkPipelineShaderStageCreateInfo, 2> stage_info{};
    for (int i = 0; i < 2; ++i) {
        stage_info[i] = make<VkPipelineShaderStageCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
        stage_info[i].stage = i == 0 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
        stage_info[i].module = r.modules[i];
        stage_info[i].pName = "main";
    }
    auto vertex_input = make<VkPipelineVertexInputStateCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
    auto assembly = make<VkPipelineInputAssemblyStateCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    const VkViewport viewport{0, 0, static_cast<float>(target.width),
                              static_cast<float>(target.height), 0, 1};
    const VkRect2D scissor{{0, 0}, {target.width, target.height}};
    auto viewports = make<VkPipelineViewportStateCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
    viewports.viewportCount = viewports.scissorCount = 1;
    viewports.pViewports = &viewport;
    viewports.pScissors = &scissor;
    auto raster = make<VkPipelineRasterizationStateCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1;
    auto multisample = make<VkPipelineMultisampleStateCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    auto blend = make<VkPipelineColorBlendStateCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
    blend.attachmentCount = 1;
    blend.pAttachments = &attachment;
    auto rendering = make<VkPipelineRenderingCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO);
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &color_format;

    auto info = make<VkGraphicsPipelineCreateInfo>(VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                                   &rendering);
    info.stageCount = 2;
    info.pStages = stage_info.data();
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewports;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blend;
    info.layout = r.layout;
    check(vkCreateGraphicsPipelines(r.device, VK_NULL_HANDLE, 1, &info, nullptr, &r.pipeline),
          "vkCreateGraphicsPipelines");
}

// Moves the color image from layout `from`, after the stage and access of `src`, to layout `to`,
// before those of `dst`.
// All `levels` levels and `layers` layers.
void transition(VkCommandBuffer commands, VkImage image, VkImageLayout from, VkImageLayout to,
                std::pair<VkPipelineStageFlags2, VkAccessFlags2> src,
                std::pair<VkPipelineStageFlags2, VkAccessFlags2> dst, std::uint32_t levels = 1,
                std::uint32_t layers = 1) {
    auto barrier = make<VkImageMemoryBarrier2>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2);
    std::tie(barrier.srcStageMask, barrier.srcAccessMask) = src;
    std::tie(barrier.dstStageMask, barrier.dstAccessMask) = dst;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, layers};
    auto dependency = make<VkDependencyInfo>(VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(commands, &dependency);
}

}  // namespace

GraphicsRunner::GraphicsRunner(ComputeRunner&& base) : ComputeRunner(std::move(base)) {}

GraphicsRunner::~GraphicsRunner() = default;

std::unique_ptr<GraphicsRunner> GraphicsRunner::create(std::string& why) {
    auto base = ComputeRunner::create(why);
    if (!base) {
        return nullptr;
    }
    return std::unique_ptr<GraphicsRunner>(new GraphicsRunner(std::move(*base)));
}

void GraphicsRunner::render(std::span<const std::uint32_t> vs, std::span<const std::uint32_t> ps,
                            const DrawBlock& draw, std::uint32_t vertices,
                            const ColorTarget& target, std::span<const Texture> textures,
                            std::uint32_t samplers) {
    const VkDeviceAddress block = draw_block(draw);  // throws without guest memory
    if (target.width == 0 || target.height == 0 ||
        std::size_t{target.width} * target.height * texel_size > guest_size()) {
        throw std::logic_error("the color target doesn't fit into the guest memory");
    }
    std::vector<VkBufferImageCopy> copies;  // per texture, its levels' regions follow
    for (const Texture& t : textures) {
        std::size_t at = t.offset;
        for (std::uint32_t level = 0; level < t.levels; ++level) {
            VkBufferImageCopy& copy = copies.emplace_back();
            copy.bufferOffset = at;
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, t.layers};
            copy.imageExtent = {std::max(t.width >> level, 1u), std::max(t.height >> level, 1u), 1};
            at += std::size_t{copy.imageExtent.width} * copy.imageExtent.height * t.layers *
                  texel_size;
        }
        if (at > guest_size()) {
            throw std::logic_error("a texture doesn't fit into the guest memory");
        }
    }
    // Shared with the deferred function below, which keeps the objects until the work is done.
    const auto r = std::make_shared<Render>(device().handle());
    r->modules = {create_module(r->device, vs), create_module(r->device, ps)};
    create_image(device(), r->device, r->target, {target.width, target.height},
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    for (const Texture& t : textures) {
        create_image(device(), r->device, r->textures.emplace_back(), t,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    }
    create_layout(*r, textures.size(), samplers);
    const VkDescriptorSet set = create_set(*r, samplers);
    create_pipeline(*r, target);

    const VkCommandBuffer commands = scheduler().commands();
    const VkBufferImageCopy* copy = copies.data();
    for (std::size_t i = 0; i < textures.size(); ++i) {
        const Texture& t = textures[i];
        const VkImage image = r->textures[i].image;
        transition(commands, image, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, {VK_PIPELINE_STAGE_2_NONE, 0},
                   {VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT}, t.levels,
                   t.layers);
        vkCmdCopyBufferToImage(commands, guest_buffer(), image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, t.levels, copy);
        copy += t.levels;
        transition(commands, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   {VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT},
                   {VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT},
                   t.levels, t.layers);
    }
    transition(commands, r->target.image, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, {VK_PIPELINE_STAGE_2_NONE, 0},
               {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT});
    auto attachment = make<VkRenderingAttachmentInfo>(VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO);
    attachment.imageView = r->target.view;
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    std::copy(target.clear.begin(), target.clear.end(), attachment.clearValue.color.float32);
    auto rendering = make<VkRenderingInfo>(VK_STRUCTURE_TYPE_RENDERING_INFO);
    rendering.renderArea = {{0, 0}, {target.width, target.height}};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;
    vkCmdBeginRendering(commands, &rendering);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipeline);
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, r->layout, 1, 1, &set, 0,
                            nullptr);
    vkCmdPushConstants(commands, r->layout, stages, 0, sizeof block, &block);
    vkCmdDraw(commands, vertices, 1, 0, 0);
    vkCmdEndRendering(commands);

    transition(commands, r->target.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT},
               {VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT});
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {target.width, target.height, 1};
    vkCmdCopyImageToBuffer(commands, r->target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           guest_buffer(), 1, &region);
    scheduler().defer([r] {});
    // The scheduler's submission ends with a barrier from all writes to host reads.
    constexpr std::uint64_t timeout_ns = 30'000'000'000;
    if (!scheduler().wait(scheduler().flush(), timeout_ns)) {
        throw std::runtime_error("the render did not complete within 30 s");
    }
}

}  // namespace bb::test::vk
