// video_core built on the host layer (src/shadps4_host): the renderer's objects come up on lavapipe
// without a window and go down again cleanly (under AddressSanitizer in the debug preset). Skips
// where there is no Vulkan loader or device.

#include <dlfcn.h>
#include <unistd.h>

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <imgui.h>

#include "core/memory.h"
#include "hle/video/video_core_presenter.hpp"
#include "imgui/renderer/imgui_impl_vulkan.h"
#include "sdl_window.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/cache_storage.h"
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

// Selects lavapipe unless the caller chose drivers, and returns why video_core can't run, or an
// empty string. Asks the plain loader first, so that a missing loader or device skips instead of
// failing an assertion inside video_core.
std::string no_vulkan_reason() {
    ::setenv("VK_LOADER_DRIVERS_SELECT", "*lvp*", 0);
    void* loader = ::dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (loader == nullptr) {
        return "no Vulkan loader";
    }
    std::string reason;
    const auto get_proc =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(::dlsym(loader, "vkGetInstanceProcAddr"));
    const auto create = reinterpret_cast<PFN_vkCreateInstance>(
        get_proc != nullptr ? get_proc(nullptr, "vkCreateInstance") : nullptr);
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (create == nullptr || create(&info, nullptr, &instance) != VK_SUCCESS) {
        reason = "no Vulkan instance";
    } else {
        const auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
            get_proc(instance, "vkEnumeratePhysicalDevices"));
        uint32_t count = 0;
        enumerate(instance, &count, nullptr);
        if (count == 0) {
            reason = "no Vulkan device";
        }
        reinterpret_cast<PFN_vkDestroyInstance>(get_proc(instance, "vkDestroyInstance"))(instance,
                                                                                         nullptr);
    }
    ::dlclose(loader);
    return reason;
}

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

// A deferred scheduler (#676) gives the same results on the device as a direct one: buffer fills,
// updates from a source that changes after each call, copies, barriers, a render pass that only
// clears and an image readback, over two submits and with enough records to hand over chunks in
// the middle of a command buffer.
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

    const auto run = [&](bool deferred) {
        Vulkan::Scheduler scheduler{instance, deferred};
        const VkBufferCreateInfo buffer_ci{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = buffer_bytes,
            .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        };
        const VmaAllocationCreateInfo buffer_alloc{
            .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                     VMA_ALLOCATION_CREATE_MAPPED_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO,
            .requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        };
        VkBuffer raw_buffer{};
        VmaAllocation buffer_allocation{};
        VmaAllocationInfo buffer_info{};
        EXPECT_EQ(vmaCreateBuffer(allocator, &buffer_ci, &buffer_alloc, &raw_buffer,
                                  &buffer_allocation, &buffer_info),
                  VK_SUCCESS);
        const vk::Buffer buffer{raw_buffer};

        const VkImageCreateInfo image_ci{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_R32_UINT,
            .extent = {image_size, image_size, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        };
        const VmaAllocationCreateInfo image_alloc{.usage = VMA_MEMORY_USAGE_AUTO};
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

    const std::vector<u32> direct = run(false);
    const std::vector<u32> deferred = run(true);
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
