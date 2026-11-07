#include "gpu/runner/graphics_runner.hpp"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <utility>

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

// The objects of one render. The scheduler keeps them until the render's work has completed.
struct Render {
    VkDevice device;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    std::array<VkShaderModule, 2> modules{};
    VkPipeline pipeline = VK_NULL_HANDLE;

    ~Render() {
        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyShaderModule(device, modules[0], nullptr);
        vkDestroyShaderModule(device, modules[1], nullptr);
        vkDestroyImageView(device, view, nullptr);
        vkDestroyImage(device, image, nullptr);
        vkFreeMemory(device, memory, nullptr);
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

void create_image(const bb::vk::Device& device, Render& r, const ColorTarget& target) {
    auto info = make<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = color_format;
    info.extent = {target.width, target.height, 1};
    info.mipLevels = info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    check(vkCreateImage(r.device, &info, nullptr, &r.image), "vkCreateImage");
    VkMemoryRequirements needs;
    vkGetImageMemoryRequirements(r.device, r.image, &needs);
    auto alloc = make<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
    alloc.allocationSize = needs.size;
    alloc.memoryTypeIndex = device.memory_type(needs.memoryTypeBits, 0).value();
    check(vkAllocateMemory(r.device, &alloc, nullptr, &r.memory), "vkAllocateMemory");
    check(vkBindImageMemory(r.device, r.image, r.memory, 0), "vkBindImageMemory");
    auto view = make<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    view.image = r.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = color_format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    check(vkCreateImageView(r.device, &view, nullptr, &r.view), "vkCreateImageView");
}

void create_pipeline(Render& r, VkPipelineLayout layout, const ColorTarget& target) {
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
    info.layout = layout;
    check(vkCreateGraphicsPipelines(r.device, VK_NULL_HANDLE, 1, &info, nullptr, &r.pipeline),
          "vkCreateGraphicsPipelines");
}

// Moves the color image from layout `from`, after the stage and access of `src`, to layout `to`,
// before those of `dst`.
void transition(VkCommandBuffer commands, VkImage image, VkImageLayout from, VkImageLayout to,
                std::pair<VkPipelineStageFlags2, VkAccessFlags2> src,
                std::pair<VkPipelineStageFlags2, VkAccessFlags2> dst) {
    auto barrier = make<VkImageMemoryBarrier2>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2);
    std::tie(barrier.srcStageMask, barrier.srcAccessMask) = src;
    std::tie(barrier.dstStageMask, barrier.dstAccessMask) = dst;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    auto dependency = make<VkDependencyInfo>(VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(commands, &dependency);
}

}  // namespace

GraphicsRunner::GraphicsRunner(ComputeRunner&& base) : ComputeRunner(std::move(base)) {}

// The pipelines may outlive their layout.
GraphicsRunner::~GraphicsRunner() {
    vkDestroyPipelineLayout(device().handle(), layout_, nullptr);
}

std::unique_ptr<GraphicsRunner> GraphicsRunner::create(std::string& why) {
    auto base = ComputeRunner::create(why);
    if (!base) {
        return nullptr;
    }
    std::unique_ptr<GraphicsRunner> runner(new GraphicsRunner(std::move(*base)));
    VkPushConstantRange range{stages, 0, sizeof(VkDeviceAddress)};
    auto info = make<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &range;
    check(vkCreatePipelineLayout(runner->device().handle(), &info, nullptr, &runner->layout_),
          "vkCreatePipelineLayout");
    return runner;
}

void GraphicsRunner::render(std::span<const std::uint32_t> vs, std::span<const std::uint32_t> ps,
                            const DrawBlock& draw, std::uint32_t vertices,
                            const ColorTarget& target) {
    const VkDeviceAddress block = draw_block(draw);  // throws without guest memory
    if (target.width == 0 || target.height == 0 ||
        std::size_t{target.width} * target.height * texel_size > guest_size()) {
        throw std::logic_error("the color target doesn't fit into the guest memory");
    }
    // Shared with the deferred function below, which keeps the objects until the work is done.
    const auto r = std::make_shared<Render>(device().handle());
    r->modules = {create_module(r->device, vs), create_module(r->device, ps)};
    create_image(device(), *r, target);
    create_pipeline(*r, layout_, target);

    const VkCommandBuffer commands = scheduler().commands();
    transition(commands, r->image, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, {VK_PIPELINE_STAGE_2_NONE, 0},
               {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT});
    auto attachment = make<VkRenderingAttachmentInfo>(VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO);
    attachment.imageView = r->view;
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
    vkCmdPushConstants(commands, layout_, stages, 0, sizeof block, &block);
    vkCmdDraw(commands, vertices, 1, 0, 0);
    vkCmdEndRendering(commands);

    transition(commands, r->image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT},
               {VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT});
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {target.width, target.height, 1};
    vkCmdCopyImageToBuffer(commands, r->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           guest_buffer(), 1, &region);
    scheduler().defer([r] {});
    // The scheduler's submission ends with a barrier from all writes to host reads.
    constexpr std::uint64_t timeout_ns = 30'000'000'000;
    if (!scheduler().wait(scheduler().flush(), timeout_ns)) {
        throw std::runtime_error("the render did not complete within 30 s");
    }
}

}  // namespace bb::test::vk
