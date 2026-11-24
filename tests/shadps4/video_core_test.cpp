// video_core built on the host layer (src/shadps4_host): the renderer's objects come up on lavapipe
// without a window and go down again cleanly (under AddressSanitizer in the debug preset). Skips
// where there is no Vulkan loader or device.

#include <unistd.h>

#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <imgui.h>

#include "core/memory.h"
#include "hle/video/video_core_presenter.hpp"
#include "imgui/renderer/imgui_impl_vulkan.h"
#include "sdl_window.h"
#include "support/vulkan_probe.hpp"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_copy_queue.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"

#include <vk_mem_alloc.h>

namespace {

// These tests run without the on-disk pipeline cache, so they never read or write the user's
// cache directory. The cache's own test (video_core_test.cpp) opens a directory of its own.
[[maybe_unused]] const bool no_disk_cache = (Storage::pipeline_cache_enabled = false);

// What ImGui::Core::Initialize sets up and ~Instance tears down (ImGui::Core::Shutdown), without
// the window parts: the context, the Vulkan backend and the texture worker.
void start_imgui(const Vulkan::Instance& instance) {
    ImGui::CreateContext();
    // Shutdown frees both names with delete[], as Initialize allocates them. No files here.
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().LogFilename = nullptr;
    static constexpr vk::Format format = vk::Format::eB8G8R8A8Unorm;
    ImGui::Vulkan::InitInfo info{};
    info.instance = instance.GetInstance();
    info.physical_device = instance.GetPhysicalDevice();
    info.device = instance.GetDevice();
    info.queue_family = instance.GetPresentQueueFamilyIndex();
    info.queue = instance.GetPresentQueue();
    info.image_count = 2;
    info.min_allocation_size = 1024 * 1024;
    info.pipeline_rendering_create_info.colorAttachmentCount = 1;
    info.pipeline_rendering_create_info.pColorAttachmentFormats = &format;
    ASSERT_TRUE(ImGui::Vulkan::Init(info));
}

vk::ArrayWrapper1D<std::uint8_t, VK_UUID_SIZE> device_uuid(vk::PhysicalDevice device) {
    const auto chain =
        device.getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceIDProperties>();
    return chain.get<vk::PhysicalDeviceIDProperties>().deviceUUID;
}

}  // namespace

TEST(VideoCore, CreatesAndDestroysRendererOnLavapipe) {
    if (const std::string reason = no_vulkan_reason(); !reason.empty()) {
        GTEST_SKIP() << reason;
    }
    // In the order the presenter (vk_presenter.cpp) and the GNM driver create them, and destroyed
    // in reverse. The GPU thread starts with Liverpool and stops with it.
    Core::MemoryManager memory;
    auto liverpool = std::make_unique<AmdGpu::Liverpool>(memory);
    Frontend::WindowSDL window;
    {
        Vulkan::Instance instance{window, -1};
        EXPECT_TRUE(instance.GetDevice());
        if (std::string(std::getenv("VK_LOADER_DRIVERS_SELECT")) == "*lvp*") {
            EXPECT_EQ(instance.GetDriverID(), vk::DriverId::eMesaLlvmpipe);
        }
        start_imgui(instance);
        {
            Vulkan::Scheduler draw_scheduler{instance};
            Vulkan::Scheduler present_scheduler{instance};
            Vulkan::Scheduler flip_scheduler{instance};
            Vulkan::Runtime runtime{instance, draw_scheduler};
            Vulkan::Rasterizer rasterizer{instance, draw_scheduler, runtime, liverpool.get()};

            // A submission goes through the device and completes.
            const u64 tick = draw_scheduler.CurrentTick();
            draw_scheduler.Finish();
            EXPECT_GT(draw_scheduler.CurrentTick(), tick);
            EXPECT_TRUE(draw_scheduler.IsFree(tick));
        }
    }
    liverpool.reset();
}

// Liverpool tells the pipeline lookup's register tracker (#780, pipeline_regs.h) about a changed
// register the lookup reads, through each kind of SET packet and CLEAR_STATE, and not about user
// data, a rewritten value or a register the lookup does not read. Needs no Vulkan device.
TEST(VideoCore, LiverpoolTracksTheRegistersOfThePipelineLookup) {
    Core::MemoryManager memory;
    auto liverpool = std::make_unique<AmdGpu::Liverpool>(memory);
    const auto type3 = [](u32 opcode, u32 dwords) { return 3u << 30 | (dwords - 2) << 16 | opcode << 8; };
    // Runs one packet and returns whether the generation moved.
    const auto moves = [&](std::vector<u32> packet) {
        const u64 before = liverpool->pipeline_regs.Generation();
        liverpool->SubmitGfx(packet, {});
        liverpool->WaitGpuIdle();
        return liverpool->pipeline_regs.Generation() != before;
    };
    constexpr u32 SetContextReg = 0x69;
    constexpr u32 SetShReg = 0x76;
    constexpr u32 SetUconfigReg = 0x79;
    constexpr u32 ClearState = 0x12;
    const auto word = [](size_t byte_offset) { return static_cast<u32>(byte_offset / 4); };
    const u32 shader_control = word(offsetof(AmdGpu::Regs, depth_shader_control)) -
                               AmdGpu::Regs::ContextRegWordOffset;
    const u32 depth_bounds = word(offsetof(AmdGpu::Regs, depth_bounds_min)) -
                             AmdGpu::Regs::ContextRegWordOffset;
    const u32 gs_program = word(offsetof(AmdGpu::Regs, gs_program)) - AmdGpu::Regs::ShRegWordOffset;
    const u32 vs_user_data = word(offsetof(AmdGpu::Regs, vs_program.user_data)) -
                             AmdGpu::Regs::ShRegWordOffset;
    const u32 primitive_type = word(offsetof(AmdGpu::Regs, primitive_type)) -
                               AmdGpu::Regs::UconfigRegWordOffset;
    const u32 esgs_ring = word(offsetof(AmdGpu::Regs, vgt_esgs_ring_size)) -
                          AmdGpu::Regs::UconfigRegWordOffset;

    EXPECT_TRUE(moves({type3(SetContextReg, 3), shader_control, 0x10}));
    EXPECT_FALSE(moves({type3(SetContextReg, 3), shader_control, 0x10}));  // the same value
    EXPECT_FALSE(moves({type3(SetContextReg, 3), depth_bounds, 0x3f000000}));  // not read
    EXPECT_TRUE(moves({type3(SetShReg, 4), gs_program, 0x1234, 0x1}));
    EXPECT_FALSE(moves({type3(SetShReg, 6), vs_user_data, 1, 2, 3, 4}));
    EXPECT_FALSE(moves({type3(SetUconfigReg, 3), primitive_type, 4}));  // read at every draw
    EXPECT_FALSE(moves({type3(SetUconfigReg, 3), esgs_ring, 0x100}));  // not read
    EXPECT_TRUE(moves({type3(ClearState, 2), 0}));
    liverpool.reset();
}

// The staging ring keeps its first 12 blocks of 16 MiB however long they are idle (#531): a
// fresh block costs a page fault per page on its first write. The blocks after those go once
// they have been idle for 300 frames.
TEST(VideoCore, StagingRingKeepsItsFirstBlocks) {
    if (const std::string reason = no_vulkan_reason(); !reason.empty()) {
        GTEST_SKIP() << reason;
    }
    Core::MemoryManager memory;
    auto liverpool = std::make_unique<AmdGpu::Liverpool>(memory);
    Frontend::WindowSDL window;
    Vulkan::Instance instance{window, -1};
    start_imgui(instance);
    {
        Vulkan::Scheduler scheduler{instance};
        Vulkan::StagingBufferPool pool{instance, scheduler};
        constexpr u64 block = u64{16} << 20;
        constexpr auto type = VideoCore::MemoryType::HostUncached;
        // Whole blocks in one tick that has not completed: each takes a new block.
        for (int i = 0; i < 14; ++i) {
            const auto ref = pool.Request(block, type);
            ASSERT_NE(ref.mapped, nullptr);
            ref.mapped[block - 1] = 1;
        }
        EXPECT_EQ(pool.RingBlocks(type), 14u);
        scheduler.Finish();
        for (int frame = 0; frame < 299; ++frame) {
            pool.TickFrame();
        }
        EXPECT_EQ(pool.RingBlocks(type), 14u);
        pool.TickFrame();
        EXPECT_EQ(pool.RingBlocks(type), 12u);
        for (int frame = 0; frame < 1000; ++frame) {
            pool.TickFrame();
        }
        EXPECT_EQ(pool.RingBlocks(type), 12u);
    }
    liverpool.reset();
}

// Deferred operations run in order once the GPU passed their tick, and the scheduler queries the
// work semaphore only when the first operation waits for a submitted tick (#589). A submission
// gated on a host-signalled semaphore keeps the last known GPU tick behind until the test opens
// the gate and waits on the device itself, so a skipped query shows as a stale known tick.
TEST(VideoCore, PendingOperationsPollOnlyWhenOneWaits) {
    if (const std::string reason = no_vulkan_reason(); !reason.empty()) {
        GTEST_SKIP() << reason;
    }
    Core::MemoryManager memory;
    auto liverpool = std::make_unique<AmdGpu::Liverpool>(memory);
    Frontend::WindowSDL window;
    Vulkan::Instance instance{window, -1};
    start_imgui(instance);
    {
        const vk::Device device = instance.GetDevice();
        Vulkan::Scheduler scheduler{instance};
        Vulkan::Semaphore& work = *scheduler.GetWorkSemaphore();
        Vulkan::Semaphore gate{instance};
        std::vector<int> ran;
        // Submits the current tick behind the gate, opens it and waits for the tick through the
        // device, not through the scheduler, which would refresh the known tick.
        const auto submit_gated = [&] {
            const u64 tick = scheduler.CurrentTick();
            Vulkan::SubmitInfo info{};
            info.AddWait(gate.Handle(), tick);
            scheduler.Flush(info);
            EXPECT_LT(work.KnownGpuTick(), tick);
            const vk::SemaphoreSignalInfo signal{.semaphore = gate.Handle(), .value = tick};
            EXPECT_EQ(device.signalSemaphore(signal), vk::Result::eSuccess);
            const vk::Semaphore handle = work.Handle();
            const vk::SemaphoreWaitInfo wait{
                .semaphoreCount = 1, .pSemaphores = &handle, .pValues = &tick};
            EXPECT_EQ(device.waitSemaphores(wait, UINT64_MAX), vk::Result::eSuccess);
            return tick;
        };

        // Nothing pending: no query, the known tick stays behind the device.
        const u64 first = submit_gated();
        scheduler.PopPendingOperations();
        EXPECT_LT(work.KnownGpuTick(), first);

        // Only an operation of the unsubmitted tick: no query either, and it doesn't run.
        scheduler.DeferOperation([&] { ran.push_back(1); });
        scheduler.PopPendingOperations();
        EXPECT_LT(work.KnownGpuTick(), first);
        EXPECT_TRUE(ran.empty());

        // Its tick submitted and complete: one query frees it; the later one waits.
        const u64 second = submit_gated();
        scheduler.DeferOperation([&] { ran.push_back(2); });
        scheduler.PopPendingOperations();
        EXPECT_GE(work.KnownGpuTick(), second);
        EXPECT_EQ(ran, (std::vector<int>{1}));

        // Two operations freed by one query run in the order they were deferred.
        scheduler.DeferOperation([&] { ran.push_back(3); });
        const u64 third = submit_gated();
        EXPECT_LT(work.KnownGpuTick(), third);
        scheduler.PopPendingOperations();
        EXPECT_EQ(ran, (std::vector<int>{1, 2, 3}));
        scheduler.Finish();
    }
    liverpool.reset();
}

// A deferred scheduler (#676), with or without a recording thread, gives the same results on the
// device as a direct one: buffer fills, updates from a source that changes after each call,
// copies, barriers, a render pass that only clears and an image readback, over two submits and
// with enough records to hand over chunks in the middle of a command buffer.
TEST(VideoCore, DeferredRecordingMatchesDirectOnTheDevice) {
    if (const std::string reason = no_vulkan_reason(); !reason.empty()) {
        GTEST_SKIP() << reason;
    }
    Core::MemoryManager memory;
    auto liverpool = std::make_unique<AmdGpu::Liverpool>(memory);
    Frontend::WindowSDL window;
    Vulkan::Instance instance{window, -1};
    start_imgui(instance);
    const vk::Device device = instance.GetDevice();
    const VmaAllocator allocator = instance.GetAllocator();
    constexpr u32 buffer_bytes = 64 * 1024;
    constexpr u32 updates = 4000;
    constexpr u32 image_size = 8;
    constexpr u32 image_at = 40960;

    const auto run = [&](Vulkan::Recording recording) {
        Vulkan::Scheduler scheduler{instance, recording};
        // C structs: value-initialised, then set, so no field goes without an initialiser.
        VkBufferCreateInfo buffer_ci{};
        buffer_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_ci.size = buffer_bytes;
        buffer_ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo buffer_alloc{};
        buffer_alloc.flags =
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        buffer_alloc.usage = VMA_MEMORY_USAGE_AUTO;
        buffer_alloc.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        VkBuffer raw_buffer{};
        VmaAllocation buffer_allocation{};
        VmaAllocationInfo buffer_info{};
        EXPECT_EQ(vmaCreateBuffer(allocator, &buffer_ci, &buffer_alloc, &raw_buffer,
                                  &buffer_allocation, &buffer_info),
                  VK_SUCCESS);
        const vk::Buffer buffer{raw_buffer};

        VkImageCreateInfo image_ci{};
        image_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_ci.imageType = VK_IMAGE_TYPE_2D;
        image_ci.format = VK_FORMAT_R32_UINT;
        image_ci.extent = {image_size, image_size, 1};
        image_ci.mipLevels = 1;
        image_ci.arrayLayers = 1;
        image_ci.samples = VK_SAMPLE_COUNT_1_BIT;
        image_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo image_alloc{};
        image_alloc.usage = VMA_MEMORY_USAGE_AUTO;
        VkImage raw_image{};
        VmaAllocation image_allocation{};
        EXPECT_EQ(vmaCreateImage(allocator, &image_ci, &image_alloc, &raw_image,
                                 &image_allocation, nullptr),
                  VK_SUCCESS);
        const vk::Image image{raw_image};
        constexpr vk::ImageSubresourceRange color{
            .aspectMask = vk::ImageAspectFlagBits::eColor, .levelCount = 1, .layerCount = 1};
        const vk::ImageView view =
            device
                .createImageView({.image = image,
                                  .viewType = vk::ImageViewType::e2D,
                                  .format = vk::Format::eR32Uint,
                                  .subresourceRange = color})
                .value;

        const auto memory_barrier = [&](vk::PipelineStageFlags2 src, vk::PipelineStageFlags2 dst) {
            const vk::MemoryBarrier2 barrier{.srcStageMask = src,
                                             .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                                             .dstStageMask = dst,
                                             .dstAccessMask = vk::AccessFlagBits2::eMemoryRead |
                                                              vk::AccessFlagBits2::eMemoryWrite};
            scheduler.CommandBuffer().pipelineBarrier2(
                {.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
        };
        const auto image_barrier = [&](vk::ImageLayout from, vk::ImageLayout to) {
            const vk::ImageMemoryBarrier2 barrier{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .dstAccessMask =
                    vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                .oldLayout = from,
                .newLayout = to,
                .image = image,
                .subresourceRange = color};
            scheduler.CommandBuffer().pipelineBarrier2(
                {.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
        };

        auto cmdbuf = scheduler.CommandBuffer();
        cmdbuf.fillBuffer(buffer, 0, 4096, 0x11111111u);
        memory_barrier(vk::PipelineStageFlagBits2::eAllTransfer,
                       vk::PipelineStageFlagBits2::eAllTransfer);
        u32 value = 0;
        for (u32 i = 0; i < updates; ++i) {
            value = i * 3;
            cmdbuf.updateBuffer(buffer, 4096 + 4 * i, sizeof(value), &value);
        }
        value = 0xDEADBEEF;
        scheduler.Flush();

        cmdbuf = scheduler.CommandBuffer();
        memory_barrier(vk::PipelineStageFlagBits2::eAllTransfer,
                       vk::PipelineStageFlagBits2::eAllTransfer);
        cmdbuf.copyBuffer(buffer, buffer, vk::BufferCopy{.srcOffset = 4096, .dstOffset = 32768,
                                                         .size = 1024});
        image_barrier(vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal);
        Vulkan::RenderState state{};
        state.color_attachments[0].image_view = view;
        state.color_attachments[0].image_layout = vk::ImageLayout::eColorAttachmentOptimal;
        state.color_attachments[0].clear_value = {7, 8, 9, 10};
        state.color_attachments[0].is_clear = 1;
        state.num_color_attachments = 1;
        state.width = image_size;
        state.height = image_size;
        state.num_layers = 1;
        scheduler.BeginRendering(state);
        scheduler.EndRendering();
        image_barrier(vk::ImageLayout::eColorAttachmentOptimal,
                      vk::ImageLayout::eTransferSrcOptimal);
        cmdbuf.copyImageToBuffer(
            image, vk::ImageLayout::eTransferSrcOptimal, buffer,
            vk::BufferImageCopy{.bufferOffset = image_at,
                                .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                                     .layerCount = 1},
                                .imageExtent = {image_size, image_size, 1}});
        memory_barrier(vk::PipelineStageFlagBits2::eAllTransfer,
                       vk::PipelineStageFlagBits2::eHost);
        scheduler.Finish();

        const auto* words = static_cast<const u32*>(buffer_info.pMappedData);
        std::vector<u32> result(words, words + buffer_bytes / 4);
        device.destroyImageView(view);
        vmaDestroyImage(allocator, image, image_allocation);
        vmaDestroyBuffer(allocator, buffer, buffer_allocation);
        return result;
    };

    const std::vector<u32> direct = run(Vulkan::Recording::Direct);
    const std::vector<u32> deferred = run(Vulkan::Recording::Deferred);
    const std::vector<u32> threaded = run(Vulkan::Recording::Threaded);
    EXPECT_EQ(direct[0], 0x11111111u);
    EXPECT_EQ(direct[1023], 0x11111111u);
    for (u32 i = 0; i < updates; i += 997) {
        EXPECT_EQ(direct[1024 + i], i * 3) << i;
    }
    EXPECT_EQ(direct[1024 + updates - 1], (updates - 1) * 3);
    EXPECT_EQ(direct[32768 / 4 + 255], 255u * 3);
    EXPECT_EQ(direct[image_at / 4], 7u);
    EXPECT_EQ(direct[image_at / 4 + image_size * image_size - 1], 7u);
    EXPECT_TRUE(deferred == direct);
    EXPECT_TRUE(threaded == direct);
    liverpool.reset();
}

// The recording thread (#676) across the GPU thread's waits: a submit with no records, Finish
// after chunks handed over mid-buffer, a deferred operation that runs only once its tick passed,
// the present thread's wait for a tick the recording thread has not submitted yet (held back on
// submit_mutex), and a scheduler destroyed with a chunk and a submit still queued.
TEST(VideoCore, RecordingThreadSubmitsInOrderAndOnShutdown) {
    if (const std::string reason = no_vulkan_reason(); !reason.empty()) {
        GTEST_SKIP() << reason;
    }
    Core::MemoryManager memory;
    auto liverpool = std::make_unique<AmdGpu::Liverpool>(memory);
    Frontend::WindowSDL window;
    Vulkan::Instance instance{window, -1};
    start_imgui(instance);
    const VmaAllocator allocator = instance.GetAllocator();
    constexpr u32 buffer_bytes = 64 * 1024;
    constexpr u32 updates = 6000; // about 3 hand-overs of 64 KiB

    VkBufferCreateInfo buffer_ci{};
    buffer_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_ci.size = buffer_bytes;
    buffer_ci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo buffer_alloc{};
    buffer_alloc.flags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    buffer_alloc.usage = VMA_MEMORY_USAGE_AUTO;
    buffer_alloc.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkBuffer raw_buffer{};
    VmaAllocation buffer_allocation{};
    VmaAllocationInfo buffer_info{};
    ASSERT_EQ(vmaCreateBuffer(allocator, &buffer_ci, &buffer_alloc, &raw_buffer,
                              &buffer_allocation, &buffer_info),
              VK_SUCCESS);
    const vk::Buffer buffer{raw_buffer};
    const auto* words = static_cast<const volatile u32*>(buffer_info.pMappedData);
    const auto host_barrier = [&](const Vulkan::DeferredCommandBuffer& cmdbuf) {
        const vk::MemoryBarrier2 barrier{.srcStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
                                         .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                                         .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                                         .dstAccessMask = vk::AccessFlagBits2::eHostRead};
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
    };
    Vulkan::Semaphore outside{instance};
    const auto counter = [&] {
        return instance.GetDevice().getSemaphoreCounterValue(outside.Handle()).value;
    };
    {
        auto scheduler_owner =
            std::make_unique<Vulkan::Scheduler>(instance, Vulkan::Recording::Threaded);
        Vulkan::Scheduler& scheduler = *scheduler_owner;

        // Submits with no records.
        scheduler.Flush();
        scheduler.Flush();
        scheduler.Finish();
        EXPECT_TRUE(scheduler.IsFree(scheduler.CurrentTick() - 1));

        // Finish waits for chunks handed over in the middle of the command buffer.
        auto cmdbuf = scheduler.CommandBuffer();
        for (u32 i = 0; i < updates; ++i) {
            const u32 value = i + 1;
            cmdbuf.updateBuffer(buffer, 4 * (i % (buffer_bytes / 4)), sizeof(value), &value);
        }
        host_barrier(cmdbuf);
        scheduler.Finish();
        EXPECT_EQ(words[0], 1u);
        EXPECT_EQ(words[updates - 1], updates);

        // A deferred operation runs only once its tick passed.
        int ran = 0;
        cmdbuf = scheduler.CommandBuffer();
        cmdbuf.fillBuffer(buffer, 0, 4, 0xABCDu);
        host_barrier(cmdbuf);
        scheduler.DeferOperation([&] { ++ran; });
        const u64 tick = scheduler.CurrentTick();
        scheduler.PopPendingOperations();
        EXPECT_EQ(ran, 0);

        // The present thread's wait: the recording thread can't submit while the test holds
        // submit_mutex, so WaitSubmitted blocks until it lets go.
        std::atomic<bool> submitted{false};
        std::thread present;
        {
            std::scoped_lock hold{Vulkan::Scheduler::submit_mutex};
            scheduler.Flush();
            present = std::thread([&] {
                scheduler.WaitSubmitted(tick);
                submitted = true;
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            EXPECT_FALSE(submitted.load());
        }
        present.join();
        EXPECT_TRUE(submitted.load());
        scheduler.Wait(tick);
        scheduler.PopPendingOperations();
        EXPECT_EQ(ran, 1);
        EXPECT_EQ(words[0], 0xABCDu);

        // Destroyed with a chunk and a submit queued: both still reach the queue. The recording
        // thread is held on submit_mutex behind an earlier submit, so the later chunk and
        // submit are still queued when the destructor starts.
        std::thread destroy;
        {
            std::scoped_lock hold{Vulkan::Scheduler::submit_mutex};
            scheduler.Flush();
            cmdbuf = scheduler.CommandBuffer();
            cmdbuf.fillBuffer(buffer, 4, 4, 0x5678u);
            host_barrier(cmdbuf);
            Vulkan::SubmitInfo info{};
            info.AddSignal(outside.Handle(), 1);
            scheduler.Flush(info);
            destroy = std::thread([&] { scheduler_owner.reset(); });
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            EXPECT_EQ(counter(), 0u);
        }
        destroy.join();
    }
    // The destructor waited for its last tick, so the signal is there without a wait.
    EXPECT_EQ(counter(), 1u);
    EXPECT_EQ(words[1], 0x5678u);
    vmaDestroyBuffer(allocator, buffer, buffer_allocation);
    liverpool.reset();
}

// A sparse arena bind while chunks are queued (#695, docs/design/command-recording.md). The
// submit callback runs on the GPU thread and calls bindSparse under submit_mutex, as
// BufferCache::SubmitPendingArenaBinds does. The test holds the recording thread on submit_mutex
// behind an earlier, empty submit, so the chunks of the commands recorded before the bind are
// queued and unsubmitted when the callback runs. Once the test lets go, the bind and that empty
// submit may take the mutex in either order; the bind still reaches the queue before the submit
// of those chunks, which waits for it. Commands recorded before the bind, on a page that was
// bound already, and after it, on the new page, both land. The test holds submit_mutex itself, so
// it does not check that the bind takes the mutex (lavapipe serialises its queue anyway). The
// buffer cache's arenas are 4 GiB, more than lavapipe's sparse address space, so the test binds
// a small sparse buffer of its own the same way.
TEST(VideoCore, SparseBindWhileChunksAreQueued) {
    if (const std::string reason = no_vulkan_reason(); !reason.empty()) {
        GTEST_SKIP() << reason;
    }
    Core::MemoryManager memory;
    auto liverpool = std::make_unique<AmdGpu::Liverpool>(memory);
    Frontend::WindowSDL window;
    Vulkan::Instance instance{window, -1};
    start_imgui(instance);
    const vk::PhysicalDeviceFeatures features = instance.GetPhysicalDevice().getFeatures();
    if (!features.sparseBinding || !features.sparseResidencyBuffer) {
        liverpool.reset();
        GTEST_SKIP() << "no sparse residency buffers on this device";
    }
    const vk::Device device = instance.GetDevice();
    const VmaAllocator allocator = instance.GetAllocator();
    constexpr u32 updates = 6000; // about 3 hand-overs of 64 KiB, all on the first page
    constexpr vk::DeviceSize buffer_bytes = 4 * 1024 * 1024;

    const vk::Buffer sparse =
        device
            .createBuffer({.flags = vk::BufferCreateFlagBits::eSparseBinding |
                                    vk::BufferCreateFlagBits::eSparseResidency,
                           .size = buffer_bytes,
                           .usage = vk::BufferUsageFlagBits::eTransferSrc |
                                    vk::BufferUsageFlagBits::eTransferDst})
            .value;
    const vk::MemoryRequirements requirements = device.getBufferMemoryRequirements(sparse);
    // A "page" here is the bind unit: whole sparse blocks that hold the updates.
    const vk::DeviceSize block = requirements.alignment;
    const vk::DeviceSize page = (vk::DeviceSize{updates} * 4 + block - 1) / block * block;
    ASSERT_LE(2 * page, buffer_bytes);
    const u32 type = static_cast<u32>(std::countr_zero(requirements.memoryTypeBits));
    std::vector<vk::DeviceMemory> pages;
    for (int i = 0; i < 2; ++i) {
        pages.push_back(device.allocateMemory({.allocationSize = page, .memoryTypeIndex = type})
                            .value);
    }
    Vulkan::Semaphore bound{instance};
    // Binds page `index` of the buffer and, given a submit, makes it wait for the bind.
    const auto bind = [&](u32 index, Vulkan::SubmitInfo* info) {
        const vk::SparseMemoryBind memory_bind{
            .resourceOffset = index * page, .size = page, .memory = pages[index]};
        const vk::SparseBufferMemoryBindInfo buffer_bind{
            .buffer = sparse, .bindCount = 1, .pBinds = &memory_bind};
        const u64 tick = bound.NextTick();
        const vk::Semaphore handle = bound.Handle();
        const vk::TimelineSemaphoreSubmitInfo timeline{.signalSemaphoreValueCount = 1,
                                                       .pSignalSemaphoreValues = &tick};
        const vk::BindSparseInfo bind_info{.pNext = &timeline,
                                           .bufferBindCount = 1,
                                           .pBufferBinds = &buffer_bind,
                                           .signalSemaphoreCount = 1,
                                           .pSignalSemaphores = &handle};
        if (info != nullptr) {
            info->AddWait(handle, tick);
        }
        std::scoped_lock lk{Vulkan::Scheduler::submit_mutex};
        EXPECT_EQ(instance.GetGraphicsQueue().bindSparse(bind_info), vk::Result::eSuccess);
        return tick;
    };
    bound.Wait(bind(0, nullptr));

    VkBufferCreateInfo readback_ci{};
    readback_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    readback_ci.size = 2 * updates * 4;
    readback_ci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo readback_alloc{};
    readback_alloc.flags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    readback_alloc.usage = VMA_MEMORY_USAGE_AUTO;
    readback_alloc.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkBuffer raw_readback{};
    VmaAllocation readback_allocation{};
    VmaAllocationInfo readback_info{};
    ASSERT_EQ(vmaCreateBuffer(allocator, &readback_ci, &readback_alloc, &raw_readback,
                              &readback_allocation, &readback_info),
              VK_SUCCESS);
    const vk::Buffer readback{raw_readback};
    const auto* words = static_cast<const volatile u32*>(readback_info.pMappedData);

    {
        Vulkan::Scheduler scheduler{instance, Vulkan::Recording::Threaded};
        std::atomic<bool> bind_next{false};
        std::atomic<bool> bound_in_callback{false};
        scheduler.SetSubmitCallback([&](Vulkan::SubmitInfo& info) {
            if (bind_next.exchange(false)) {
                bind(1, &info);
                bound_in_callback = true;
            }
        });
        const auto barrier = [&](vk::PipelineStageFlags2 dst, vk::AccessFlags2 dst_access) {
            const vk::MemoryBarrier2 memory_barrier{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
                .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                .dstStageMask = dst,
                .dstAccessMask = dst_access};
            scheduler.CommandBuffer().pipelineBarrier2(
                {.memoryBarrierCount = 1, .pMemoryBarriers = &memory_barrier});
        };

        std::thread gpu;
        {
            // The recording thread can't submit while the test holds submit_mutex, so the
            // chunks recorded after the first flush are still queued when the bind is made.
            std::scoped_lock hold{Vulkan::Scheduler::submit_mutex};
            gpu = std::thread([&] {
                scheduler.Flush();
                auto cmdbuf = scheduler.CommandBuffer();
                for (u32 i = 0; i < updates; ++i) {
                    const u32 value = i + 1;
                    cmdbuf.updateBuffer(sparse, 4 * i, sizeof(value), &value);
                }
                bind_next = true;
                scheduler.Flush();

                // No wait of its own: like the buffer cache's later submits, it relies on the
                // submit before it having waited for the bind.
                cmdbuf = scheduler.CommandBuffer();
                cmdbuf.fillBuffer(sparse, page, updates * 4, 0x600DF00Du);
                barrier(vk::PipelineStageFlagBits2::eAllTransfer,
                        vk::AccessFlagBits2::eTransferRead);
                const std::array copies{
                    vk::BufferCopy{.srcOffset = 0, .dstOffset = 0, .size = updates * 4},
                    vk::BufferCopy{.srcOffset = page, .dstOffset = updates * 4,
                                   .size = updates * 4}};
                cmdbuf.copyBuffer(sparse, readback, copies);
                barrier(vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostRead);
                scheduler.Finish();
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            EXPECT_FALSE(bound_in_callback.load());
        }
        gpu.join();
        EXPECT_TRUE(bound_in_callback.load());
        for (u32 i = 0; i < updates; i += 599) {
            EXPECT_EQ(words[i], i + 1) << i;
            EXPECT_EQ(words[updates + i], 0x600DF00Du) << i;
        }
        EXPECT_EQ(words[updates - 1], updates);
        EXPECT_EQ(words[2 * updates - 1], 0x600DF00Du);
    }
    vmaDestroyBuffer(allocator, readback, readback_allocation);
    device.destroyBuffer(sparse);
    for (const vk::DeviceMemory memory_page : pages) {
        device.freeMemory(memory_page);
    }
    liverpool.reset();
}

// The copy threads' tickets (#777): jobs finish out of order across threads, and Completed()
// moves past a ticket only once it and every earlier one are done.
TEST(CopyQueue, CompletesTicketsInOrder) {
    Vulkan::CopyQueue copies{2};
    EXPECT_EQ(copies.Issued(), 0u);
    copies.Wait(0); // nothing to wait for
    std::atomic<bool> release_first{false};
    std::atomic<bool> second_ran{false};
    const u64 first = copies.Push([&] {
        while (!release_first.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    const u64 second = copies.Push([&] { second_ran = true; });
    EXPECT_EQ(first, 1u);
    EXPECT_EQ(second, 2u);
    EXPECT_EQ(copies.Issued(), 2u);
    while (!second_ran.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(copies.Completed(), 0u); // the second is done, the first is not
    std::thread waiter([&] { copies.Wait(second); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    release_first = true;
    waiter.join();
    EXPECT_EQ(copies.Completed(), 2u);
}

// A queue destroyed with jobs queued runs them all before its threads stop.
TEST(CopyQueue, RunsQueuedJobsAtShutdown) {
    std::atomic<int> ran{0};
    {
        Vulkan::CopyQueue copies{2};
        for (int i = 0; i < 64; ++i) {
            copies.Push([&] {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                ++ran;
            });
        }
    }
    EXPECT_EQ(ran.load(), 64);
}

// A submit waits for the copy jobs pushed before it was handed over (#777): a copy from staging
// space a job fills reads what the job wrote, in every recording mode. With a recording thread the
// GPU thread's flush does not wait for the job; with --sync-recording (Deferred) and direct
// recording the flush submits on the calling thread and waits there.
TEST(VideoCore, ReplayWaitsForItsCopies) {
    if (const std::string reason = no_vulkan_reason(); !reason.empty()) {
        GTEST_SKIP() << reason;
    }
    Core::MemoryManager memory;
    auto liverpool = std::make_unique<AmdGpu::Liverpool>(memory);
    Frontend::WindowSDL window;
    Vulkan::Instance instance{window, -1};
    start_imgui(instance);
    const VmaAllocator allocator = instance.GetAllocator();
    constexpr u32 bytes = 64 * 1024;
    struct HostBuffer {
        VkBuffer buffer{};
        VmaAllocation allocation{};
        u8* mapped{};
    };
    const auto host_buffer = [&](VkBufferUsageFlags usage) {
        VkBufferCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        ci.size = bytes;
        ci.usage = usage;
        VmaAllocationCreateInfo alloc{};
        alloc.flags =
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        alloc.usage = VMA_MEMORY_USAGE_AUTO;
        alloc.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        HostBuffer b;
        VmaAllocationInfo info{};
        EXPECT_EQ(vmaCreateBuffer(allocator, &ci, &alloc, &b.buffer, &b.allocation, &info),
                  VK_SUCCESS);
        b.mapped = static_cast<u8*>(info.pMappedData);
        return b;
    };
    HostBuffer staging = host_buffer(VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    HostBuffer readback = host_buffer(VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    for (const auto mode : {Vulkan::Recording::Direct, Vulkan::Recording::Deferred,
                            Vulkan::Recording::Threaded}) {
        SCOPED_TRACE(static_cast<int>(mode));
        std::memset(staging.mapped, 0, bytes);
        std::memset(readback.mapped, 0, bytes);
        const u8 fill = 0x40 + static_cast<u8>(mode);
        Vulkan::Scheduler scheduler{instance, mode};
        std::atomic<bool> release{false};
        std::atomic<bool> copied{false};
        scheduler.Copies().Push([&] {
            while (!release.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            std::memset(staging.mapped, fill, bytes);
            copied = true;
        });
        auto cmdbuf = scheduler.CommandBuffer();
        cmdbuf.copyBuffer(vk::Buffer{staging.buffer}, vk::Buffer{readback.buffer},
                          vk::BufferCopy{0, 0, bytes});
        const vk::MemoryBarrier2 barrier{.srcStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
                                         .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                                         .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                                         .dstAccessMask = vk::AccessFlagBits2::eHostRead};
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
        std::thread releaser;
        if (mode == Vulkan::Recording::Threaded) {
            // The flush only queues the chunk and the submit: the GPU thread goes on.
            scheduler.Flush();
            EXPECT_FALSE(copied.load());
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            EXPECT_FALSE(scheduler.IsFree(scheduler.CurrentTick() - 1));
            release = true;
        } else {
            // The flush submits on this thread, so it waits for the job.
            releaser = std::thread([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                release = true;
            });
            scheduler.Flush();
            EXPECT_TRUE(copied.load());
        }
        scheduler.Finish();
        if (releaser.joinable()) {
            releaser.join();
        }
        EXPECT_EQ(readback.mapped[0], fill);
        EXPECT_EQ(readback.mapped[bytes - 1], fill);
    }
    vmaDestroyBuffer(allocator, staging.buffer, staging.allocation);
    vmaDestroyBuffer(allocator, readback.buffer, readback.allocation);
    liverpool.reset();
}

// --device: a name part, in any case, gives the index under which the Presenter's own instance,
// created for the window's system type, holds the named device; no match gives nothing and names
// the devices; no selector leaves the choice to the instance.
TEST(VideoCore, ResolvesTheDeviceSelectorToTheIndexInThePresentersInstance) {
    if (const std::string reason = no_vulkan_reason(); !reason.empty()) {
        GTEST_SKIP() << reason;
    }
    Frontend::WindowSDL window;
    const Frontend::WindowSystemType type = window.GetWindowInfo().type;
    std::string why;
    EXPECT_EQ(bb::hle::video::resolve_gpu_id("", type, why), -1);
    // The last device, so that the index is not 0 where there are several.
    vk::PhysicalDeviceProperties last;
    vk::ArrayWrapper1D<std::uint8_t, VK_UUID_SIZE> last_uuid;
    {
        const vk::UniqueInstance instance = Vulkan::CreateInstance(type, {});
        const auto [result, devices] = instance->enumeratePhysicalDevices();
        ASSERT_EQ(result, vk::Result::eSuccess);
        ASSERT_FALSE(devices.empty());
        last = devices.back().getProperties();
        last_uuid = device_uuid(devices.back());
    }
    std::string name = last.deviceName.data();
    for (char& c : name) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    const std::optional<std::int32_t> id = bb::hle::video::resolve_gpu_id(name, type, why);
    ASSERT_TRUE(id.has_value()) << why;
    {
        // The instance the Presenter creates with that index (vk_presenter.cpp) uses this device.
        Vulkan::Instance instance{window, *id};
        start_imgui(instance);  // ~Instance shuts ImGui down
        const vk::PhysicalDeviceProperties chosen = instance.GetPhysicalDevice().getProperties();
        EXPECT_STRCASEEQ(chosen.deviceName.data(), last.deviceName.data());
        EXPECT_EQ(device_uuid(instance.GetPhysicalDevice()), last_uuid);
    }

    EXPECT_EQ(bb::hle::video::resolve_gpu_id("no such device 0x5ca1ab1e", type, why),
              std::nullopt);
    EXPECT_NE(why.find(last.deviceName.data()), std::string::npos) << why;
}

// The disk cache across runs: the first run writes it, the next loads it, and a cache that is
// truncated, damaged, of an older schema or from another driver is dropped and rebuilt without a
// crash, with one warning, and the run after that loads clean again.
TEST(VideoCore, PipelineCacheSurvivesDamageAndOldVersions) {
    if (const std::string reason = no_vulkan_reason(); !reason.empty()) {
        GTEST_SKIP() << reason;
    }
    namespace fs = std::filesystem;
    const fs::path dir =
        fs::temp_directory_path() / ("bb-pipeline-cache-" + std::to_string(::getpid()));
    fs::remove_all(dir);
    Core::MemoryManager memory;
    auto liverpool = std::make_unique<AmdGpu::Liverpool>(memory);
    Frontend::WindowSDL window;
    Vulkan::Instance instance{window, -1};
    start_imgui(instance);

    // One run: opens the store on `dir` under `schema`, creates the renderer's PipelineCache and
    // destroys it. Returns how many files the store dropped.
    const auto run = [&](u32 schema) {
        auto& db = Storage::DataBase::Instance();
        EXPECT_TRUE(db.Open(dir, schema));
        u32 dropped = 0;
        {
            Vulkan::Scheduler scheduler{instance};
            Vulkan::PipelineCache cache{instance, scheduler, liverpool.get()};
            dropped = db.DroppedBlobs();
        }
        EXPECT_FALSE(db.IsOpened()); // the destructor saved and closed it
        return dropped;
    };
    const u32 schema = Vulkan::CacheSchema();

    EXPECT_EQ(run(schema), 0u); // cold
    ASSERT_TRUE(fs::exists(dir / "profile.bin"));
    ASSERT_TRUE(fs::exists(dir / "vk_pipeline_cache.vkc"));
    EXPECT_EQ(run(schema), 0u); // warm: everything loads

    // A cache written under another schema (an older ShaderMetaVersion): dropped, then rebuilt.
    EXPECT_EQ(run(schema + 1), 2u); // the driver cache and the profile
    EXPECT_EQ(run(schema + 1), 0u);

    // Truncated and damaged files.
    fs::resize_file(dir / "vk_pipeline_cache.vkc", fs::file_size(dir / "vk_pipeline_cache.vkc") / 2);
    {
        std::fstream f{dir / "profile.bin", std::ios::in | std::ios::out | std::ios::binary};
        f.seekp(-1, std::ios::end);
        f.put('\x7f');
    }
    EXPECT_EQ(run(schema + 1), 2u);
    EXPECT_EQ(run(schema + 1), 0u);

    // A driver cache of another device is not handed to this one: the file is valid, the key
    // inside it is not.
    {
        auto& db = Storage::DataBase::Instance();
        ASSERT_TRUE(db.Open(dir, schema + 1));
        db.Save(Storage::BlobType::DriverCache, "vk_pipeline_cache",
                std::vector<u8>(256, 0xab));
        db.Close();
    }
    {
        auto& db = Storage::DataBase::Instance();
        ASSERT_TRUE(db.Open(dir, schema + 1));
        {
            Vulkan::Scheduler scheduler{instance};
            Vulkan::PipelineCache cache{instance, scheduler, liverpool.get()};
            EXPECT_TRUE(cache.LoadDriverCache().empty());
        }
    }
    EXPECT_EQ(run(schema + 1), 0u);
    fs::remove_all(dir);
}
