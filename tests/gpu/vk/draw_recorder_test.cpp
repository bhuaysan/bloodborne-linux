// Draws through the parser, the pipeline cache, dynamic rendering and the memory mirror on
// lavapipe (docs/design/graphics.md, item 27). vs_attrs and ps_discard (the render test's pair)
// draw one triangle over the whole 16 x 8 target; the pixels with x of at least 0.5 get the
// interpolated attribute (x, y, 0.25, 1), the others are discarded and keep the cleared value.
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "gpu/gcn/programs.hpp"
#include "gpu/pm4/packet.hpp"
#include "gpu/pm4/parser.hpp"
#include "gpu/vk/device.hpp"
#include "gpu/vk/dispatcher.hpp"
#include "gpu/vk/draw_recorder.hpp"
#include "gpu/vk/image_cache.hpp"
#include "gpu/vk/memory_mirror.hpp"
#include "gpu/vk/pipeline_cache.hpp"
#include "gpu/vk/rendering.hpp"
#include "gpu/vk/scheduler.hpp"
#include "hle/kernel/dmem.hpp"
#include "hle/kernel/vmem.hpp"
#include "si_ci_vi_merged_offset.h"
#include "test_device.hpp"
#include "tools/mirror_observer.hpp"

namespace {

using namespace bb::hle::kernel;
using namespace bb::vk;
using bb::pm4::Parser;
using bb::pm4::RegisterMirror;
using bb::pm4::RegSpace;
using bb::pm4::RunResult;
using bb::pm4::Stream;
namespace amd = Pal::Gfx6::Chip;

constexpr std::uint64_t kib = 1024;
constexpr std::uint64_t vertices_at = 0x1000, indices_at = 0x2000;
constexpr std::uint64_t vs_code = 0x1000, ps_code = 0x2000;  // the pipeline cache's fake addresses
constexpr std::uint32_t width = 16, height = 8, sh_base = 0x2C00, context_base = 0xA000;

void packet(std::vector<std::uint32_t>& out, std::uint8_t opcode,
            std::initializer_list<std::uint32_t> body) {
    out.push_back(bb::pm4::type3_header(opcode, static_cast<std::uint32_t>(body.size())));
    out.insert(out.end(), body);
}

class VkDrawRecorder : public testing::Test {
protected:
    void SetUp() override {
        const std::string selector = bb::test::vk_test_device();
        std::string why;
        instance_ = Instance::create(why);
        if (!instance_) {
            GTEST_SKIP() << "no Vulkan instance: " << why;
        }
        device_ = Device::create(*instance_, {.device = selector}, why);
        if (!device_) {
            GTEST_SKIP() << "no Vulkan device: " << why;
        }
        ASSERT_GE(dmem::fd(), 0);
        mirror_ = MemoryMirror::create(*device_,
                                       {.memfd = dmem::fd(),
                                        .window_start = vmem::window_start,
                                        .size = mirror_buffer_size,
                                        .min_block = 64 * kib},
                                       why);
        if (!mirror_) {
            GTEST_SKIP() << "no mirror: " << why;
        }
        observer_ = std::make_unique<bb::tools::MirrorObserver>(*mirror_);
        vmem::set_direct_observer(observer_.get());
        std::uint64_t offset = ~std::uint64_t{0};
        ASSERT_EQ(allocate_direct(0, direct_memory_size(), 256 * kib, 64 * kib, 0, &offset), 0);
        offset_ = offset;
        ASSERT_EQ(map_direct(&memory_, 256 * kib, 3, 0, offset, 64 * kib), 0);
        guest_ = reinterpret_cast<std::uintptr_t>(memory_);
        std::memset(memory_, 0, 256 * kib);
        // The triangle (-1,-1), (3,-1), (-1,3) covers the target; the attribute is 0 at the
        // first vertex and 2 in x and y at the others (render_test.cpp).
        const float vertices[3][8] = {{-1, -1, 0, 1, 0, 0, 0.25f, 1},
                                      {3, -1, 0, 1, 2, 0, 0.25f, 1},
                                      {-1, 3, 0, 1, 0, 2, 0.25f, 1}};
        std::memcpy(bytes() + vertices_at, vertices, sizeof vertices);

        code_[vs_code] = bb::test::gcn::program_words("vs_attrs");
        code_[ps_code] = bb::test::gcn::program_words("ps_discard");
        scheduler_ = std::make_unique<Scheduler>(*device_);
        compute_ = std::make_unique<ComputePipelineCache>(*device_, reader());
        dispatcher_ = std::make_unique<Dispatcher>(*device_, *scheduler_, *compute_, *mirror_);
        images_ = std::make_unique<ImageCache>(*device_);
        targets_ = std::make_unique<TargetRenderer>(*device_, *images_);
        pipelines_ = std::make_unique<PipelineCache>(*device_, reader());
        recorder_ = std::make_unique<DrawRecorder>(*device_, *scheduler_, *dispatcher_,
                                                   *pipelines_, *targets_, *mirror_, hints_);
        parser_ = std::make_unique<Parser>(regs_);
        parser_->set_draw([this](const RegisterMirror& r, const bb::pm4::DrawCall& c) {
            recorder_->draw(r, c);
        });
        parser_->set_sync([this](bb::pm4::SyncPoint p) { dispatcher_->sync(p); });
        setup_registers();
    }

    void TearDown() override {
        recorder_.reset();
        dispatcher_.reset();  // waits for the GPU before the memory goes
        if (memory_ != nullptr) {
            EXPECT_EQ(sceKernelMunmap(memory_, 256 * kib), 0);
            EXPECT_EQ(release_direct(offset_, 256 * kib), 0);
        }
        vmem::set_direct_observer(nullptr);
    }

    CodeReader reader() {
        return [this](std::uint64_t address) -> std::span<const std::uint32_t> {
            const auto it = code_.find(address);
            return it != code_.end() ? std::span(it->second) : std::span<std::uint32_t>();
        };
    }
    std::byte* bytes() const { return static_cast<std::byte*>(memory_); }
    void sh(std::uint32_t reg, std::initializer_list<std::uint32_t> values) {
        regs_.write(RegSpace::sh, reg - sh_base, std::span(values.begin(), values.size()));
    }
    void context(std::uint32_t reg, std::uint32_t value) {
        regs_.write(RegSpace::context, reg - context_base, std::span(&value, 1));
    }

    // The registers of the render test's pair drawing into colour target 0: 16 x 8 8_8_8_8
    // UNORM whose CMASK is marked cleared (to 0) before each run.
    void setup_registers() {
        const std::uint64_t at = guest_ + vertices_at;
        sh(amd::mmSPI_SHADER_PGM_LO_VS, {vs_code >> 8, 0, 3u << 24, 4u << 1});
        sh(amd::mmSPI_SHADER_USER_DATA_VS_0, {static_cast<std::uint32_t>(at),
                                              static_cast<std::uint32_t>(at >> 32) | 32u << 16, 3, 0});
        sh(amd::mmSPI_SHADER_PGM_LO_PS, {ps_code >> 8, 0, 0, 0});
        context(amd::mmVGT_INSTANCE_STEP_RATE_0, 1);
        context(amd::mmVGT_INSTANCE_STEP_RATE_1, 2);
        context(amd::mmSPI_PS_INPUT_ENA, 2);
        context(amd::mmSPI_PS_INPUT_ADDR, 2);
        context(amd::mmSPI_SHADER_COL_FORMAT, 9);  // 32_ABGR
        const std::uint32_t triangle_list = 4;  // a uconfig register
        regs_.write(RegSpace::uconfig, amd::mmVGT_PRIMITIVE_TYPE__CI__VI - 0xC000,
                    std::span(&triangle_list, 1));
        context(amd::mmPA_CL_VTE_CNTL, 0x43F);
        for (const auto [reg, value] : {std::pair{amd::mmPA_CL_VPORT_XSCALE, 8.0f},
                                        {amd::mmPA_CL_VPORT_XOFFSET, 8.0f},
                                        {amd::mmPA_CL_VPORT_YSCALE, 4.0f},
                                        {amd::mmPA_CL_VPORT_YOFFSET, 4.0f},
                                        {amd::mmPA_CL_VPORT_ZSCALE, 0.5f},
                                        {amd::mmPA_CL_VPORT_ZOFFSET, 0.5f}}) {
            context(reg, std::bit_cast<std::uint32_t>(value));
        }
        for (const auto reg : {amd::mmPA_SC_SCREEN_SCISSOR_BR, amd::mmPA_SC_GENERIC_SCISSOR_BR,
                               amd::mmPA_SC_WINDOW_SCISSOR_BR}) {
            context(reg, height << 16 | width);
        }
        context(amd::mmCB_COLOR0_BASE, 0x1000);
        context(amd::mmCB_COLOR0_PITCH, 1);
        context(amd::mmCB_COLOR0_SLICE, 1);
        context(amd::mmCB_COLOR0_VIEW, 0);
        context(amd::mmCB_COLOR0_INFO, 10u << 2 | 1u << 13);  // 8_8_8_8 UNORM, FAST_CLEAR
        context(amd::mmCB_COLOR0_CMASK, 0x2000);
        context(amd::mmCB_COLOR0_CLEAR_WORD0, 0);
        context(amd::mmCB_TARGET_MASK, 0xF);
        context(amd::mmCB_COLOR_CONTROL, 1u << 4);
    }

    RunResult run(const std::vector<std::uint32_t>& stream) {
        targets_->mark_cleared(0x200000);  // the next rendering clears the target to 0
        Stream s(stream);
        const RunResult result = parser_->run(s);
        dispatcher_->sync(bb::pm4::SyncPoint::before_write);
        return result;
    }

    // The target's texels, 4 bytes each, copied out of its image.
    std::vector<std::uint8_t> read() {
        Image* image = images_->get(*color_image_key(*bb::pm4::color_target(regs_, hints_, 0)));
        const VkDevice device = device_->handle();
        const VkDeviceSize size = width * height * 4;
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
        b.oldLayout = image->layout;
        b.newLayout = image->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image->handle;
        b.subresourceRange = {image->aspects, 0, 1, 0, image->key.slices};
        auto dependency = make<VkDependencyInfo>(VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(commands, &dependency);
        const VkBufferImageCopy region{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {}, {width, height, 1}};
        vkCmdCopyImageToBuffer(commands, image->handle, image->layout, buffer, 1, &region);
        scheduler_->finish();
        std::vector<std::uint8_t> texels(size);
        void* data;
        check(vkMapMemory(device, memory, 0, size, 0, &data), "vkMapMemory");
        std::memcpy(texels.data(), data, size);
        vkUnmapMemory(device, memory);
        vkDestroyBuffer(device, buffer, nullptr);
        vkFreeMemory(device, memory, nullptr);
        return texels;
    }

    // Whether the target holds the triangle in columns [first, last) and 0 elsewhere.
    ::testing::AssertionResult triangle_in(std::uint32_t first, std::uint32_t last) {
        const auto texels = read();
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                const std::uint8_t* t = &texels[(y * width + x) * 4];
                const bool drawn = x >= std::max(first, width / 2) && x < last;
                const float expect[4] = {(x + 0.5f) / width, (y + 0.5f) / height, 0.25f, 1};
                for (int c = 0; c < 4; ++c) {
                    const float want = drawn ? expect[c] * 255 : 0;
                    if (std::abs(t[c] - want) > 1.5f) {
                        return ::testing::AssertionFailure()
                               << "texel (" << x << ", " << y << ") channel " << c << " is "
                               << int(t[c]) << ", expected " << want;
                    }
                }
            }
        }
        return ::testing::AssertionSuccess();
    }

    std::unique_ptr<Instance> instance_;
    std::unique_ptr<Device> device_;
    std::unique_ptr<MemoryMirror> mirror_;
    std::unique_ptr<bb::tools::MirrorObserver> observer_;
    std::map<std::uint64_t, std::vector<std::uint32_t>> code_;
    std::unique_ptr<Scheduler> scheduler_;
    std::unique_ptr<ComputePipelineCache> compute_;
    std::unique_ptr<Dispatcher> dispatcher_;
    std::unique_ptr<ImageCache> images_;
    std::unique_ptr<TargetRenderer> targets_;
    std::unique_ptr<PipelineCache> pipelines_;
    bb::pm4::ExtentHints hints_;
    std::unique_ptr<DrawRecorder> recorder_;
    RegisterMirror regs_;
    std::unique_ptr<Parser> parser_;
    void* memory_ = nullptr;
    std::uint64_t offset_ = 0;
    std::uintptr_t guest_ = 0;
};

TEST_F(VkDrawRecorder, AutoDrawRendersTheTriangle) {
    std::vector<std::uint32_t> stream;
    packet(stream, IT_DRAW_INDEX_AUTO, {3, 2});
    ASSERT_EQ(run(stream), RunResult::done);
    EXPECT_TRUE(triangle_in(0, width));
    EXPECT_EQ(recorder_->stats().recorded, 1u);
    EXPECT_EQ(recorder_->stats().unsupported, 0u);
    EXPECT_EQ(recorder_->stats().failed, 0u);
    EXPECT_EQ(pipelines_->size(), 1u);
}

TEST_F(VkDrawRecorder, IndexedDrawsReadTheIndicesThroughTheMirror) {
    // 16-bit indices (2, 0, 1) of DRAW_INDEX_2, then 32-bit indices of DRAW_INDEX_OFFSET_2 from
    // INDEX_BASE that skip the first one: (9, 1, 2, 0) from index 1.
    const std::uint16_t narrow[] = {2, 0, 1};
    const std::uint32_t wide[] = {9, 1, 2, 0};
    std::memcpy(bytes() + indices_at, narrow, sizeof narrow);
    std::memcpy(bytes() + indices_at + 0x100, wide, sizeof wide);
    const std::uint64_t base = guest_ + indices_at;
    const std::uint64_t base_wide = base + 0x100;
    for (const bool is_wide : {false, true}) {
        std::vector<std::uint32_t> stream;
        if (!is_wide) {
            packet(stream, IT_INDEX_TYPE, {0});
            packet(stream, IT_DRAW_INDEX_2,
                   {3, static_cast<std::uint32_t>(base), static_cast<std::uint32_t>(base >> 32), 3, 0});
        } else {
            packet(stream, IT_INDEX_TYPE, {1});
            packet(stream, IT_INDEX_BASE, {static_cast<std::uint32_t>(base_wide),
                                           static_cast<std::uint32_t>(base_wide >> 32)});
            packet(stream, IT_DRAW_INDEX_OFFSET_2, {4, 1, 3, 0});
        }
        ASSERT_EQ(run(stream), RunResult::done);
        EXPECT_TRUE(triangle_in(0, width)) << (is_wide ? "32-bit" : "16-bit");
        EXPECT_EQ(recorder_->stats().failed, 0u);
    }
    EXPECT_EQ(recorder_->stats().recorded, 2u);
}

TEST_F(VkDrawRecorder, RestartNeedsTheAllOnesIndexOfTheIndexType) {
    context(amd::mmVGT_MULTI_PRIM_IB_RESET_EN, 1);
    const std::uint32_t triangle_strip = 6;
    regs_.write(RegSpace::uconfig, amd::mmVGT_PRIMITIVE_TYPE__CI__VI - 0xC000,
                std::span(&triangle_strip, 1));
    const std::uint64_t base = guest_ + indices_at;
    const std::uint32_t lo = static_cast<std::uint32_t>(base), hi = base >> 32;
    const std::uint16_t narrow[] = {2, 0, 1};
    const std::uint32_t wide[] = {2, 0, 1};
    struct Case {
        std::uint32_t reset, type;
        bool recorded;
    };
    // The index type is 0 for 16-bit and 1 for 32-bit indices.
    const Case cases[] = {{0xffff, 1, false}, {0xffffffff, 0, false},
                          {0xffff, 0, true},  {0xffffffff, 1, true}};
    std::uint64_t recorded = 0, unsupported = 0;
    for (const Case& c : cases) {
        context(amd::mmVGT_MULTI_PRIM_IB_RESET_INDX, c.reset);
        if (c.type == 0) {
            std::memcpy(bytes() + indices_at, narrow, sizeof narrow);
        } else {
            std::memcpy(bytes() + indices_at, wide, sizeof wide);
        }
        std::vector<std::uint32_t> stream;
        packet(stream, IT_INDEX_TYPE, {c.type});
        packet(stream, IT_DRAW_INDEX_2, {3, lo, hi, 3, 0});
        ASSERT_EQ(run(stream), RunResult::done);
        (c.recorded ? recorded : unsupported)++;
        EXPECT_EQ(recorder_->stats().recorded, recorded);
        EXPECT_EQ(recorder_->stats().unsupported, unsupported);
        EXPECT_EQ(recorder_->stats().failed, 0u);
        if (c.recorded) {
            EXPECT_TRUE(triangle_in(0, width)) << std::hex << c.reset;
        }
    }
}

TEST_F(VkDrawRecorder, ARestartIndexInTheDataEndsTheStrip) {
    // Strip (1, 0, restart, 0, 1, 2) with back faces culled. After the restart the triangle
    // (0, 1, 2) is front-facing. Without it, the fourth triangle of the strip would be the
    // odd one, (1, 0, 2), which faces back and is culled; the others use the zero vertex of the
    // restart index. So only a real restart draws anything.
    context(amd::mmPA_SU_SC_MODE_CNTL, 6);  // cull back, clockwise is front
    context(amd::mmVGT_MULTI_PRIM_IB_RESET_EN, 1);
    context(amd::mmVGT_MULTI_PRIM_IB_RESET_INDX, 0xffff);
    const std::uint32_t triangle_strip = 6;
    regs_.write(RegSpace::uconfig, amd::mmVGT_PRIMITIVE_TYPE__CI__VI - 0xC000,
                std::span(&triangle_strip, 1));
    const std::uint16_t data[] = {1, 0, 0xffff, 0, 1, 2};
    std::memcpy(bytes() + indices_at, data, sizeof data);
    const std::uint64_t base = guest_ + indices_at;
    std::vector<std::uint32_t> stream;
    packet(stream, IT_INDEX_TYPE, {0});
    packet(stream, IT_DRAW_INDEX_2, {6, static_cast<std::uint32_t>(base),
                                     static_cast<std::uint32_t>(base >> 32), 6, 0});
    ASSERT_EQ(run(stream), RunResult::done);
    EXPECT_EQ(recorder_->stats().recorded, 1u);
    EXPECT_TRUE(triangle_in(0, width));
}

TEST_F(VkDrawRecorder, IndicesPastTheMirrorBufferFailTheDraw) {
    // 2^29 32-bit indices fill a whole buffer; the buffer starts after the window's first byte.
    const std::uint64_t base = guest_ + indices_at;
    std::vector<std::uint32_t> stream;
    packet(stream, IT_INDEX_TYPE, {1});
    packet(stream, IT_DRAW_INDEX_2, {1u << 29, static_cast<std::uint32_t>(base),
                                     static_cast<std::uint32_t>(base >> 32), 1u << 29, 0});
    ASSERT_EQ(run(stream), RunResult::done);
    EXPECT_EQ(recorder_->stats().failed, 1u);
    EXPECT_EQ(recorder_->stats().recorded, 0u);
}

TEST_F(VkDrawRecorder, ScissorLimitsTheDraw) {
    context(amd::mmPA_SC_GENERIC_SCISSOR_BR, height << 16 | 12);  // columns 0-11
    std::vector<std::uint32_t> stream;
    packet(stream, IT_DRAW_INDEX_AUTO, {3, 2});
    ASSERT_EQ(run(stream), RunResult::done);
    EXPECT_TRUE(triangle_in(0, 12));
}

TEST_F(VkDrawRecorder, UnsupportedDrawsAreCountedAndTheStreamGoesOn) {
    // Two unsupported draws (no colour target; a restart index that isn't all ones) and then a
    // supported one, which draws. A strip of three vertices is the same triangle as a list.
    const std::uint32_t triangle_strip = 6;
    regs_.write(RegSpace::uconfig, amd::mmVGT_PRIMITIVE_TYPE__CI__VI - 0xC000,
                std::span(&triangle_strip, 1));
    std::vector<std::uint32_t> stream;
    packet(stream, IT_SET_CONTEXT_REG, {amd::mmCB_TARGET_MASK - context_base, 0});  // no target
    packet(stream, IT_DRAW_INDEX_AUTO, {3, 2});
    packet(stream, IT_SET_CONTEXT_REG, {amd::mmCB_TARGET_MASK - context_base, 0xF});
    packet(stream, IT_SET_CONTEXT_REG, {amd::mmVGT_MULTI_PRIM_IB_RESET_EN - context_base, 1});
    packet(stream, IT_SET_CONTEXT_REG, {amd::mmVGT_MULTI_PRIM_IB_RESET_INDX - context_base, 0x1234});
    packet(stream, IT_DRAW_INDEX_AUTO, {3, 2});
    packet(stream, IT_SET_CONTEXT_REG, {amd::mmVGT_MULTI_PRIM_IB_RESET_EN - context_base, 0});
    packet(stream, IT_DRAW_INDEX_AUTO, {3, 2});
    ASSERT_EQ(run(stream), RunResult::done);
    EXPECT_EQ(recorder_->stats().unsupported, 2u);
    EXPECT_EQ(recorder_->stats().recorded, 1u);
    EXPECT_EQ(recorder_->stats().failed, 0u);
    EXPECT_TRUE(triangle_in(0, width));
}

TEST_F(VkDrawRecorder, SyncBeforeWaitSubmitsOnceAndThenNothing) {
    std::vector<std::uint32_t> stream;
    packet(stream, IT_DRAW_INDEX_AUTO, {3, 2});
    Stream s(stream);
    ASSERT_EQ(parser_->run(s), RunResult::done);
    ASSERT_TRUE(scheduler_->recording());
    dispatcher_->sync(bb::pm4::SyncPoint::before_wait);
    const std::uint64_t submitted = scheduler_->submitted();
    EXPECT_FALSE(scheduler_->recording());
    EXPECT_GE(submitted, 1u);
    // A waiting packet retries its sync point every poll: nothing recorded, nothing submitted.
    for (int poll = 0; poll < 3; ++poll) {
        dispatcher_->sync(bb::pm4::SyncPoint::before_wait);
    }
    EXPECT_EQ(scheduler_->submitted(), submitted);
}

TEST_F(VkDrawRecorder, BlendingIntoAnIntegerTargetDrawsWithoutBlending) {
    // 8_8_8_8 UINT can't blend in Vulkan (no COLOR_ATTACHMENT_BLEND) and the hardware ignores the
    // blend state there: the pipeline is made with blendEnable cleared. With ONE + ONE a second
    // draw over the first would add up if it blended; it leaves the texels as they were. (llvmpipe
    // skips the blend of integer formats itself, so the pipeline cache reports the blendEnable it
    // made the pipeline with; a driver that applies it would show the sum.)
    VkFormatProperties properties;
    vkGetPhysicalDeviceFormatProperties(device_->physical(), VK_FORMAT_R8G8B8A8_UINT, &properties);
    ASSERT_EQ(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT, 0u);
    context(amd::mmCB_COLOR0_INFO, 4u << 8 | 10u << 2 | 1u << 13);
    context(amd::mmCB_BLEND0_CONTROL, 1u << 30 | 1u << 0 | 1u << 8 | 1u << 16 | 1u << 24);
    std::vector<std::uint32_t> once, twice;
    packet(once, IT_DRAW_INDEX_AUTO, {3, 2});
    twice = once;
    packet(twice, IT_DRAW_INDEX_AUTO, {3, 2});
    ASSERT_EQ(run(once), RunResult::done);
    const auto key = color_image_key(*bb::pm4::color_target(regs_, hints_, 0));
    ASSERT_TRUE(key);
    EXPECT_EQ(key->format, VK_FORMAT_R8G8B8A8_UINT);
    EXPECT_EQ(pipelines_->last_blend_enable(), 0u);
    const std::vector<std::uint8_t> single = read();
    ASSERT_EQ(run(twice), RunResult::done);
    EXPECT_EQ(recorder_->stats().recorded, 3u);
    EXPECT_EQ(recorder_->stats().failed, 0u);
    EXPECT_EQ(read(), single);
    EXPECT_NE(single, std::vector<std::uint8_t>(single.size()));  // something was drawn
}

TEST_F(VkDrawRecorder, BlendingIntoATargetThatCanBlendKeepsBlendEnable) {
    // The same blend state on the UNORM target stays on; this is what the test above clears.
    context(amd::mmCB_BLEND0_CONTROL, 1u << 30 | 1u << 0 | 1u << 8 | 1u << 16 | 1u << 24);
    std::vector<std::uint32_t> stream;
    packet(stream, IT_DRAW_INDEX_AUTO, {3, 2});
    ASSERT_EQ(run(stream), RunResult::done);
    EXPECT_EQ(recorder_->stats().recorded, 1u);
    EXPECT_EQ(pipelines_->last_blend_enable(), 1u);
}

}  // namespace
