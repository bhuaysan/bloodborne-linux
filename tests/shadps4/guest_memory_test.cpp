// The guest memory video_core sees (src/shadps4_host/memory.cpp): the backing is the direct memory
// memfd, its page table follows the kernel's mappings of direct and flexible memory, and the
// rasterizer hears of each GPU-visible map and unmap.

#include <dlfcn.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <imgui.h>

#include "core/memory.h"
#include "hle/kernel/dmem.hpp"
#include "hle/kernel/vmem.hpp"
#include "imgui/renderer/imgui_impl_vulkan.h"
#include "imgui/renderer/texture_manager.h"
#include "sdl_window.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace {

using namespace bb::hle::kernel;
using vmem::page_size;

std::uintptr_t addr(const void* p) {
    return reinterpret_cast<std::uintptr_t>(p);
}

Core::MemoryManager& memory() {
    return *Core::Memory::Instance();
}

// The backing entry of the page that holds `address`, or null.
u8* entry(std::uintptr_t address) {
    const auto& pages = memory().GetAddressSpace().BackingPages();
    const auto* e = pages.find(address >> Core::GUEST_PAGE_BITS);
    return e != nullptr ? *e : nullptr;
}

// Direct memory, mapped where the kernel places it.
struct Direct {
    std::uint64_t offset = 0;
    void* address = nullptr;
    std::uint64_t length = 0;
    explicit Direct(std::uint64_t pages) : length(pages * page_size) {
        EXPECT_EQ(allocate_direct(0, direct_memory_size(), length, 0, 0, &offset), 0);
        EXPECT_EQ(map_direct(&address, length, 3, 0, offset, 0), 0);
    }
    void unmap() {
        EXPECT_EQ(sceKernelMunmap(address, length), 0);
        EXPECT_EQ(release_direct(offset, length), 0);
    }
};

void* flexible(std::uint64_t pages) {
    void* p = nullptr;
    EXPECT_EQ(map_anonymous(&p, pages * page_size, 3, 0), 0);
    return p;
}

// Writes through the backing land in the guest mapping and the reverse, across a page boundary,
// and every page points at its own place in the memfd.
void round_trip(std::uintptr_t start, std::uint64_t backing) {
    const std::uintptr_t at = start + page_size - 4;
    EXPECT_TRUE(memory().IsValidMapping(at, 8));
    EXPECT_TRUE(memory().TryWriteBacking(reinterpret_cast<void*>(at), "backing", 8));
    EXPECT_STREQ(reinterpret_cast<const char*>(at), "backing");
    std::strcpy(reinterpret_cast<char*>(at), "guest");
    char read[8] = {'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x'};
    memory().CopySparseMemory(at, reinterpret_cast<u8*>(read), sizeof read);
    EXPECT_STREQ(read, "guest");
    u8* const base = memory().GetAddressSpace().BackingBase();
    EXPECT_EQ(entry(start), base + backing);
    EXPECT_EQ(entry(start + page_size), base + backing + page_size);
}

TEST(GuestMemory, DirectMemoryIsBacked) {
    memory();
    Direct d(2);
    round_trip(addr(d.address), d.offset);
    const std::uintptr_t start = addr(d.address);
    d.unmap();
    EXPECT_EQ(entry(start), nullptr);
    EXPECT_EQ(entry(start + page_size), nullptr);
}

TEST(GuestMemory, FlexibleMemoryIsBacked) {
    memory();
    void* const p = flexible(2);
    const std::uint64_t backing = vmem::find(addr(p))->backing;
    ASSERT_GE(backing, dmem::total_size);
    round_trip(addr(p), backing);
    // A partial unmap removes the entries of its page only.
    EXPECT_EQ(sceKernelMunmap(p, page_size), 0);
    EXPECT_EQ(entry(addr(p)), nullptr);
    EXPECT_NE(entry(addr(p) + page_size), nullptr);
    EXPECT_EQ(sceKernelMunmap(static_cast<char*>(p) + page_size, page_size), 0);
    EXPECT_EQ(entry(addr(p) + page_size), nullptr);
}

// A reservation has no backing: sparse reads give zeros and nothing is written.
TEST(GuestMemory, UnbackedPagesReadZerosAndTakeNoWrites) {
    memory();
    void* range = nullptr;
    ASSERT_EQ(reserve_range(&range, page_size, 0, 0), 0);
    EXPECT_TRUE(memory().IsValidMapping(addr(range), page_size));
    EXPECT_FALSE(memory().TryWriteBacking(range, "x", 2));
    std::vector<u8> read(16, 0xcc);
    memory().CopySparseMemory(addr(range), read.data(), read.size());
    EXPECT_EQ(read, std::vector<u8>(16, 0));
    EXPECT_EQ(sceKernelMunmap(range, page_size), 0);
    EXPECT_FALSE(memory().IsValidMapping(addr(range), page_size));
    EXPECT_FALSE(memory().IsValidMapping(addr(range)));
}

// A buffer of a GiB or more is clamped to the mapped regions that follow each other from its
// start; a reservation ends them.
TEST(GuestMemory, LargeRangesAreClampedToTheMappedRegions) {
    memory();
    constexpr std::uint64_t gib = std::uint64_t{1} << 30;
    void* range = nullptr;
    ASSERT_EQ(reserve_range(&range, 2 * gib, 0, 0), 0);
    std::uint64_t offset = 0;
    ASSERT_EQ(allocate_direct(0, direct_memory_size(), gib, 0, 0, &offset), 0);
    const BatchEntry halves[2] = {
        {addr(range), offset, gib / 2, 3, {}, dmem::batch_map},
        {addr(range) + gib / 2, offset + gib / 2, gib / 2, 3, {}, dmem::batch_map},
    };
    ASSERT_EQ(map_direct_batch(halves, 2, nullptr), 0);
    EXPECT_EQ(memory().ClampRangeSize(addr(range), 2 * gib), gib);
    EXPECT_EQ(memory().ClampRangeSize(addr(range) + 0x100, gib), gib - 0x100);
    EXPECT_EQ(memory().ClampRangeSize(addr(range) + 0x100, gib - 1), gib - 1);  // not clamped
    EXPECT_EQ(sceKernelMunmap(range, 2 * gib), 0);
    EXPECT_EQ(release_direct(offset, gib), 0);
}

// The coverage bb-run logs: every GPU-visible region is backed page by page, and the table holds
// no other entries.
TEST(GuestMemory, CoverageCountsTheBackedRegions) {
    const Core::MemoryManager::Coverage before = memory().BackingCoverage();
    EXPECT_EQ(before.covered, before.regions);
    EXPECT_EQ(before.table_pages, before.pages);
    Direct d(3);
    void* const p = flexible(2);
    const Core::MemoryManager::Coverage during = memory().BackingCoverage();
    EXPECT_EQ(during.regions, before.regions + 2);
    EXPECT_EQ(during.covered, during.regions);
    EXPECT_EQ(during.pages, before.pages + 5);
    EXPECT_EQ(during.table_pages, during.pages);
    d.unmap();
    EXPECT_EQ(sceKernelMunmap(p, 2 * page_size), 0);
    const Core::MemoryManager::Coverage after = memory().BackingCoverage();
    EXPECT_EQ(after.regions, before.regions);
    EXPECT_EQ(after.table_pages, before.table_pages);
}

// Selects lavapipe unless the caller chose drivers, and returns why video_core can't run, or an
// empty string (as video_core_test.cpp).
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

// What the rasterizer needs of ImGui, as in video_core_test.cpp.
void start_imgui(const Vulkan::Instance& instance) {
    ImGui::CreateContext();
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
    ImGui::Core::TextureManager::StartWorker();
}

// The rasterizer hears of GPU-visible maps and unmaps of direct and flexible memory, and of
// nothing once it unregistered.
TEST(GuestMemory, TheRasterizerFollowsTheMappings) {
    if (const std::string reason = no_vulkan_reason(); !reason.empty()) {
        GTEST_SKIP() << reason;
    }
    auto liverpool = std::make_unique<AmdGpu::Liverpool>();
    Frontend::WindowSDL window;
    {
        Vulkan::Instance instance{window, -1};
        start_imgui(instance);
        Vulkan::Scheduler scheduler{instance};
        Vulkan::Runtime runtime{instance, scheduler};
        Vulkan::Rasterizer rasterizer{instance, scheduler, runtime, liverpool.get()};
        Direct d(2);
        void* const p = flexible(1);
        EXPECT_TRUE(rasterizer.IsMapped(addr(d.address), d.length));
        EXPECT_TRUE(rasterizer.IsMapped(addr(p), page_size));
        d.unmap();
        EXPECT_FALSE(rasterizer.IsMapped(addr(d.address), d.length));
        EXPECT_TRUE(rasterizer.IsMapped(addr(p), page_size));
        EXPECT_EQ(sceKernelMunmap(p, page_size), 0);
        EXPECT_FALSE(rasterizer.IsMapped(addr(p), page_size));
        memory().SetRasterizer(nullptr);
        void* const late = flexible(1);
        EXPECT_FALSE(rasterizer.IsMapped(addr(late), page_size));
        EXPECT_EQ(sceKernelMunmap(late, page_size), 0);
    }
    liverpool.reset();
}

}  // namespace
