// Dynamic rendering into the targets of the register mirror, and the clears of register state,
// on lavapipe: a clear through register state lands in the cached image.

#include <array>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "gpu/vk/device.hpp"
#include "gpu/vk/image_cache.hpp"
#include "gpu/vk/rendering.hpp"
#include "gpu/vk/scheduler.hpp"
#include "si_ci_vi_merged_offset.h"

namespace {

using namespace bb::vk;
namespace amd = Pal::Gfx6::Chip;

class VkRendering : public testing::Test {
protected:
    void SetUp() override {
        ::setenv("VK_LOADER_DRIVERS_SELECT", "*lvp*", 0);  // as in device_test.cpp
        std::string why;
        instance_ = Instance::create(why);
        if (!instance_) {
            GTEST_SKIP() << "no Vulkan instance: " << why;
        }
        device_ = Device::create(*instance_, {.device = "llvmpipe"}, why);
        if (!device_) {
            GTEST_SKIP() << "no lavapipe device: " << why;
        }
        cache_ = std::make_unique<ImageCache>(*device_);
        renderer_ = std::make_unique<TargetRenderer>(*device_, *cache_);
        scheduler_ = std::make_unique<Scheduler>(*device_);
    }

    void set(std::uint32_t reg, std::uint32_t value) {
        regs_.write(bb::pm4::RegSpace::context, reg - 0xA000, std::span(&value, 1));
    }

    // Colour target 0: 16 x 8 8_8_8_8 UNORM with FAST_CLEAR, three slices, the view from
    // `base_slice`, the CMASK at 0x200000, normal mode with all components enabled.
    void color_target(std::uint32_t base_slice, std::uint32_t clear_word) {
        set(amd::mmCB_COLOR0_BASE, 0x1000);
        set(amd::mmCB_COLOR0_PITCH, 1);  // (1 + 1) * 8 = 16
        set(amd::mmCB_COLOR0_SLICE, 1);  // (1 + 1) * 64 / 16 = 8
        set(amd::mmCB_COLOR0_VIEW, 2u << 13 | base_slice);
        set(amd::mmCB_COLOR0_INFO, 10u << 2 | 1u << 13);
        set(amd::mmCB_COLOR0_CMASK, 0x2000);
        set(amd::mmCB_COLOR0_CLEAR_WORD0, clear_word);
        set(amd::mmCB_TARGET_MASK, 0xF);
        set(amd::mmCB_COLOR_CONTROL, 1u << 4);
    }
    Image* color_image() {
        return cache_->get(*color_image_key(*bb::pm4::color_target(regs_, hints_, 0)));
    }

    // Depth target: 16 x 8 Z32_FLOAT with stencil, depth and stencil tests and depth writes on.
    void depth_target() {
        set(amd::mmDB_Z_INFO, 3);
        set(amd::mmDB_STENCIL_INFO, 1);
        set(amd::mmDB_Z_READ_BASE, 0x3000);
        set(amd::mmDB_STENCIL_READ_BASE, 0x3800);
        set(amd::mmDB_DEPTH_SIZE, 1);  // pitch 16, height 8
        set(amd::mmDB_DEPTH_CONTROL, 0x7);
    }

    // Begins and ends the rendering of the registers' targets, then waits.
    std::optional<RenderingInfo> render() {
        const VkCommandBuffer commands = scheduler_->commands();
        const auto info = renderer_->begin(commands, regs_, hints_);
        if (info) {
            TargetRenderer::end(commands);
        }
        scheduler_->finish();
        return info;
    }

    // Every layer of `aspect` of `image`, `texel` bytes per texel, as the GPU holds it.
    std::vector<std::uint8_t> read(Image& image, VkImageAspectFlags aspect, std::uint32_t texel) {
        const VkDevice device = device_->handle();
        const VkExtent3D extent{image.key.extent.width, image.key.extent.height, 1};
        const VkDeviceSize size = VkDeviceSize{extent.width} * extent.height *
                                  image.key.slices * texel;
        auto info = make<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
        info.size = size;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkBuffer buffer;
        check(vkCreateBuffer(device, &info, nullptr, &buffer), "vkCreateBuffer");
        VkMemoryRequirements needs;
        vkGetBufferMemoryRequirements(device, buffer, &needs);
        auto alloc = make<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
        alloc.allocationSize = needs.size;
        alloc.memoryTypeIndex = *device_->memory_type(
            needs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkDeviceMemory memory;
        check(vkAllocateMemory(device, &alloc, nullptr, &memory), "vkAllocateMemory");
        check(vkBindBufferMemory(device, buffer, memory, 0), "vkBindBufferMemory");

        const VkCommandBuffer commands = scheduler_->commands();
        auto b = make<VkImageMemoryBarrier2>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2);
        b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        b.oldLayout = image.layout;
        b.newLayout = image.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image.handle;
        b.subresourceRange = {image.aspects, 0, 1, 0, image.key.slices};
        auto dependency = make<VkDependencyInfo>(VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(commands, &dependency);
        const VkBufferImageCopy region{0, 0, 0, {aspect, 0, 0, image.key.slices}, {}, extent};
        vkCmdCopyImageToBuffer(commands, image.handle, image.layout, buffer, 1, &region);
        scheduler_->finish();

        std::vector<std::uint8_t> bytes(size);
        void* data;
        check(vkMapMemory(device, memory, 0, size, 0, &data), "vkMapMemory");
        std::memcpy(bytes.data(), data, size);
        vkUnmapMemory(device, memory);
        vkDestroyBuffer(device, buffer, nullptr);
        vkFreeMemory(device, memory, nullptr);
        return bytes;
    }

    // Whether every texel of layers `first` to `last` holds `texel`.
    static bool layers_hold(std::span<const std::uint8_t> bytes, std::uint32_t layer_bytes,
                            std::uint32_t first, std::uint32_t last,
                            std::span<const std::uint8_t> texel) {
        for (std::size_t i = first * layer_bytes; i < (last + 1) * layer_bytes; ++i) {
            if (bytes[i] != texel[i % texel.size()]) {
                return false;
            }
        }
        return true;
    }

    bb::pm4::RegisterMirror regs_;
    bb::pm4::ExtentHints hints_;
    std::unique_ptr<Instance> instance_;
    std::unique_ptr<Device> device_;
    std::unique_ptr<ImageCache> cache_;
    std::unique_ptr<TargetRenderer> renderer_;
    std::unique_ptr<Scheduler> scheduler_;  // destroyed first: it waits for the GPU
};

constexpr std::uint32_t layer_bytes = 16 * 8 * 4;

TEST_F(VkRendering, ColorFastClearLandsInTheCachedImage) {
    // All three slices cleared to 0, then the view of slices 1-2 to the clear words: R 0x20,
    // G 0x40, B 0xFF, A 0x80.
    color_target(0, 0);
    renderer_->mark_cleared(0x200000);
    ASSERT_TRUE(render());
    color_target(1, 0x80FF4020);
    renderer_->mark_cleared(0x200000);
    const auto info = render();
    ASSERT_TRUE(info);
    EXPECT_EQ(info->color[0], VK_FORMAT_R8G8B8A8_UNORM);
    EXPECT_EQ(info->color_count, 1u);
    EXPECT_EQ(info->depth, VK_FORMAT_UNDEFINED);
    EXPECT_EQ(info->stencil, VK_FORMAT_UNDEFINED);
    EXPECT_EQ(info->extent.width, 16u);
    EXPECT_EQ(info->extent.height, 8u);
    EXPECT_EQ(info->layers, 2u);
    EXPECT_EQ(info->samples, 1u);
    EXPECT_FALSE(renderer_->cleared(0x200000));  // the clear took the mark

    Image* image = color_image();
    ASSERT_NE(image, nullptr);
    const std::uint8_t zero[] = {0, 0, 0, 0};
    const std::uint8_t color[] = {0x20, 0x40, 0xFF, 0x80};
    const auto bytes = read(*image, VK_IMAGE_ASPECT_COLOR_BIT, 4);
    EXPECT_TRUE(layers_hold(bytes, layer_bytes, 0, 0, zero));
    EXPECT_TRUE(layers_hold(bytes, layer_bytes, 1, 2, color));

    // Without a mark the target is loaded: the image keeps its contents.
    color_target(0, 0);
    ASSERT_TRUE(render());
    EXPECT_EQ(read(*image, VK_IMAGE_ASPECT_COLOR_BIT, 4), bytes);
}

TEST_F(VkRendering, TargetMaskAndModeChooseTheTargets) {
    color_target(0, 0);
    renderer_->mark_cleared(0x200000);
    set(amd::mmCB_TARGET_MASK, 0);  // no component of target 0
    EXPECT_FALSE(render());
    set(amd::mmCB_TARGET_MASK, 0xF);
    set(amd::mmCB_COLOR_CONTROL, 0);  // colour disabled
    EXPECT_FALSE(render());
    EXPECT_TRUE(renderer_->cleared(0x200000));  // nothing bound took the mark

    // Target 2 alone: slots 0 and 1 stay unbound.
    set(amd::mmCB_COLOR_CONTROL, 1u << 4);
    set(amd::mmCB_TARGET_MASK, 0xF00);
    for (const std::uint32_t reg : {amd::mmCB_COLOR0_BASE, amd::mmCB_COLOR0_PITCH,
                                    amd::mmCB_COLOR0_SLICE, amd::mmCB_COLOR0_INFO}) {
        set(reg + 2 * (amd::mmCB_COLOR1_BASE - amd::mmCB_COLOR0_BASE), regs_.get(reg));
    }
    const auto info = render();
    ASSERT_TRUE(info);
    EXPECT_EQ(info->color_count, 3u);
    EXPECT_EQ(info->color[0], VK_FORMAT_UNDEFINED);
    EXPECT_EQ(info->color[2], VK_FORMAT_R8G8B8A8_UNORM);
    EXPECT_EQ(info->layers, 1u);
}

TEST_F(VkRendering, FastClearCoversTheWholeView) {
    // Target 0 (16 x 8, slices 0-2) marked, next to target 1 (8 x 8, one slice): the rendering
    // covers 8 x 8 and one layer, the clear all of target 0's view.
    color_target(0, 0x80FF4020);
    renderer_->mark_cleared(0x200000);
    const std::uint32_t stride = amd::mmCB_COLOR1_BASE - amd::mmCB_COLOR0_BASE;
    set(amd::mmCB_COLOR0_BASE + stride, 0x5000);
    set(amd::mmCB_COLOR0_PITCH + stride, 0);  // 8
    set(amd::mmCB_COLOR0_SLICE + stride, 0);  // 8
    set(amd::mmCB_COLOR0_INFO + stride, 10u << 2);
    set(amd::mmCB_TARGET_MASK, 0xFF);
    const auto info = render();
    ASSERT_TRUE(info);
    EXPECT_EQ(info->extent.width, 8u);
    EXPECT_EQ(info->layers, 1u);
    Image* image = color_image();
    ASSERT_NE(image, nullptr);
    const std::uint8_t color[] = {0x20, 0x40, 0xFF, 0x80};
    EXPECT_TRUE(layers_hold(read(*image, VK_IMAGE_ASPECT_COLOR_BIT, 4), layer_bytes, 0, 2, color));
}

TEST_F(VkRendering, DepthAndStencilClearsFromRenderControl) {
    depth_target();
    set(amd::mmDB_RENDER_CONTROL, 3);  // DEPTH_CLEAR_ENABLE, STENCIL_CLEAR_ENABLE
    set(amd::mmDB_DEPTH_CLEAR, std::bit_cast<std::uint32_t>(0.25f));
    set(amd::mmDB_STENCIL_CLEAR, 0x5A);
    const auto info = render();
    ASSERT_TRUE(info);
    EXPECT_EQ(info->color_count, 0u);
    EXPECT_EQ(info->depth, VK_FORMAT_D32_SFLOAT_S8_UINT);
    EXPECT_EQ(info->stencil, VK_FORMAT_D32_SFLOAT_S8_UINT);
    Image* image = cache_->get(*depth_image_key(*bb::pm4::depth_target(regs_, hints_)));
    ASSERT_NE(image, nullptr);
    const auto quarter = std::bit_cast<std::array<std::uint8_t, 4>>(0.25f);
    const std::uint8_t stencil[] = {0x5A};
    EXPECT_TRUE(layers_hold(read(*image, VK_IMAGE_ASPECT_DEPTH_BIT, 4), layer_bytes, 0, 0,
                            quarter));
    EXPECT_TRUE(layers_hold(read(*image, VK_IMAGE_ASPECT_STENCIL_BIT, 1), 16 * 8, 0, 0,
                            stencil));

    // Without depth writes the depth clear doesn't happen; a marked HTILE clears anyway, and
    // DB_DEPTH_CLEAR above 1 is clamped.
    set(amd::mmDB_DEPTH_CONTROL, 0x3);
    set(amd::mmDB_RENDER_CONTROL, 1);
    set(amd::mmDB_DEPTH_CLEAR, std::bit_cast<std::uint32_t>(2.0f));
    ASSERT_TRUE(render());
    EXPECT_TRUE(layers_hold(read(*image, VK_IMAGE_ASPECT_DEPTH_BIT, 4), layer_bytes, 0, 0,
                            quarter));
    set(amd::mmDB_Z_INFO, 3u | 1u << 29);  // TILE_SURFACE_ENABLE
    set(amd::mmDB_HTILE_DATA_BASE, 0x4000);
    renderer_->mark_cleared(0x400000);
    ASSERT_TRUE(render());
    EXPECT_FALSE(renderer_->cleared(0x400000));
    EXPECT_TRUE(layers_hold(read(*image, VK_IMAGE_ASPECT_DEPTH_BIT, 4), layer_bytes, 0, 0,
                            std::bit_cast<std::array<std::uint8_t, 4>>(1.0f)));
    EXPECT_TRUE(layers_hold(read(*image, VK_IMAGE_ASPECT_STENCIL_BIT, 1), 16 * 8, 0, 0,
                            stencil));

    // Stencil test alone with stencil only: a stencil attachment and no depth attachment.
    set(amd::mmDB_Z_INFO, 0);
    set(amd::mmDB_DEPTH_CONTROL, 0x1);
    const auto stencil_only = render();
    ASSERT_TRUE(stencil_only);
    EXPECT_EQ(stencil_only->depth, VK_FORMAT_UNDEFINED);
    EXPECT_EQ(stencil_only->stencil, VK_FORMAT_D32_SFLOAT_S8_UINT);

    // Neither test enabled: the depth target isn't bound.
    set(amd::mmDB_DEPTH_CONTROL, 0);
    EXPECT_FALSE(render());
}

TEST_F(VkRendering, EliminatePassClearsTarget0OncePerMark) {
    color_target(0, 0x11223344);
    set(amd::mmCB_COLOR_CONTROL, 2u << 4);  // CB_ELIMINATE_FAST_CLEAR
    const VkCommandBuffer commands = scheduler_->commands();
    EXPECT_FALSE(renderer_->eliminate_fast_clear(commands, regs_, hints_));  // CMASK not marked
    renderer_->mark_cleared(0x200000);
    EXPECT_TRUE(renderer_->eliminate_fast_clear(commands, regs_, hints_));
    EXPECT_FALSE(renderer_->eliminate_fast_clear(commands, regs_, hints_));  // mark taken
    EXPECT_FALSE(render());  // MODE 2 binds no colour target
    Image* image = color_image();
    ASSERT_NE(image, nullptr);
    const std::uint8_t color[] = {0x44, 0x33, 0x22, 0x11};
    EXPECT_TRUE(layers_hold(read(*image, VK_IMAGE_ASPECT_COLOR_BIT, 4), layer_bytes, 0, 2, color));

    // Other modes don't eliminate; a normal rendering takes the mark instead.
    renderer_->mark_cleared(0x200000);
    set(amd::mmCB_COLOR_CONTROL, 1u << 4);
    EXPECT_FALSE(renderer_->eliminate_fast_clear(scheduler_->commands(), regs_, hints_));
    ASSERT_TRUE(render());
    EXPECT_FALSE(renderer_->cleared(0x200000));
    // Without FAST_CLEAR the eliminate pass leaves a mark alone.
    renderer_->mark_cleared(0x200000);
    set(amd::mmCB_COLOR0_INFO, 10u << 2);
    set(amd::mmCB_COLOR_CONTROL, 2u << 4);
    EXPECT_FALSE(renderer_->eliminate_fast_clear(scheduler_->commands(), regs_, hints_));
    EXPECT_TRUE(renderer_->cleared(0x200000));
}

}  // namespace
