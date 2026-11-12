// The guest memory video_core sees (src/shadps4_host/memory.cpp): the backing is the direct memory
// memfd, its page table follows the kernel's mappings of direct and flexible memory, and the
// rasterizer hears of each GPU-visible map and unmap. Its write tracking takes the faults on the
// pages the caches protected, and the kernel's reads into guest memory lift the protection first.

#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <imgui.h>

#include "core/libraries/videoout/buffer.h"
#include "core/memory.h"
#include "hle/kernel/dmem.hpp"
#include "hle/kernel/file.hpp"
#include "hle/kernel/vmem.hpp"
#include "imgui/renderer/imgui_impl_vulkan.h"
#include "imgui/renderer/texture_manager.h"
#include "sdl_window.h"
#include "shadps4_host/fault_handler.hpp"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

namespace {

// These tests run without the on-disk pipeline cache, so they never read or write the user's
// cache directory. The cache's own test (video_core_test.cpp) opens a directory of its own.
[[maybe_unused]] const bool no_disk_cache = (Storage::pipeline_cache_enabled = false);

using namespace bb::hle::kernel;
using vmem::page_size;

std::uintptr_t addr(const void* p) {
    return reinterpret_cast<std::uintptr_t>(p);
}

Core::MemoryManager& memory() {
    static Core::MemoryManager manager;  // created on first use, which is before any mapping
    return manager;
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
}

// As upstream's, a range is valid where it lies in the guest's address space, mapped or not: the
// guest window and the guest mappings outside it. Nothing else is.
TEST(GuestMemory, ValidRangesLieInTheGuestAddressSpace) {
    memory();
    void* range = nullptr;
    ASSERT_EQ(reserve_range(&range, page_size, 0, 0), 0);
    EXPECT_EQ(sceKernelMunmap(range, page_size), 0);
    EXPECT_TRUE(memory().IsValidMapping(addr(range)));
    EXPECT_TRUE(memory().IsValidMapping(addr(range), page_size));
    EXPECT_TRUE(memory().IsValidMapping(vmem::window_start, vmem::window_end - vmem::window_start));
    EXPECT_TRUE(memory().IsValidMapping(vmem::window_end - 1));
    ASSERT_FALSE(vmem::find(vmem::window_end).has_value());
    ASSERT_FALSE(vmem::find(vmem::window_start - 1).has_value());
    EXPECT_FALSE(memory().IsValidMapping(vmem::window_end));
    EXPECT_FALSE(memory().IsValidMapping(vmem::window_end - page_size, 2 * page_size));
    EXPECT_FALSE(memory().IsValidMapping(vmem::window_start - 1, 2));
    EXPECT_FALSE(memory().IsValidMapping(~std::uintptr_t{0} - 1, 4));  // wraps around
}

// A write or read that runs past its mapping into a reservation and free address space doesn't
// fail: the backed start is written, and the rest reads zeros.
TEST(GuestMemory, RangesPastAMappingUseTheirBackedStart) {
    memory();
    void* range = nullptr;
    ASSERT_EQ(reserve_range(&range, 3 * page_size, 0, 0), 0);
    const std::uintptr_t r = addr(range);
    std::uint64_t offset = 0;
    ASSERT_EQ(allocate_direct(0, direct_memory_size(), page_size, 0, 0, &offset), 0);
    const BatchEntry entry{r, offset, page_size, 3, {}, dmem::batch_map};
    ASSERT_EQ(map_direct_batch(&entry, 1, nullptr), 0);
    ASSERT_EQ(sceKernelMunmap(reinterpret_cast<void*>(r + 2 * page_size), page_size), 0);
    const std::uintptr_t at = r + page_size - 4;
    std::vector<u8> data(2 * page_size + 4, 0x5a);
    ASSERT_TRUE(memory().IsValidMapping(at, data.size()));
    EXPECT_TRUE(memory().TryWriteBacking(reinterpret_cast<void*>(at), data.data(), data.size()));
    EXPECT_EQ(std::memcmp(reinterpret_cast<const void*>(at), data.data(), 4), 0);
    std::vector<u8> read(data.size(), 0xcc);
    memory().CopySparseMemory(at, read.data(), read.size());
    std::vector<u8> expected(data.size(), 0);
    std::fill_n(expected.begin(), 4, u8{0x5a});
    EXPECT_EQ(read, expected);
    EXPECT_EQ(sceKernelMunmap(range, 2 * page_size), 0);
    EXPECT_EQ(release_direct(offset, page_size), 0);
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

// From free address space, as from upstream's free ranges, the clamp runs to the next region and
// then on over the mapped ones.
TEST(GuestMemory, LargeRangesFromFreeSpaceAreClampedToo) {
    memory();
    constexpr std::uint64_t gib = std::uint64_t{1} << 30;
    void* range = nullptr;
    ASSERT_EQ(reserve_range(&range, 3 * gib, 0, 0), 0);
    const std::uintptr_t r = addr(range);
    std::uint64_t offset = 0;
    ASSERT_EQ(allocate_direct(0, direct_memory_size(), gib, 0, 0, &offset), 0);
    const BatchEntry entry{r + gib, offset, gib, 3, {}, dmem::batch_map};
    ASSERT_EQ(map_direct_batch(&entry, 1, nullptr), 0);
    ASSERT_EQ(sceKernelMunmap(range, gib), 0);
    ASSERT_FALSE(vmem::find(r + 0x100).has_value());
    EXPECT_EQ(memory().ClampRangeSize(r + 0x100, 3 * gib), 2 * gib - 0x100);
    EXPECT_EQ(sceKernelMunmap(reinterpret_cast<void*>(r + gib), 2 * gib), 0);
    EXPECT_EQ(release_direct(offset, gib), 0);
}

// The coverage bb-run logs: every GPU-visible region is backed page by page, and the table holds
// no other entries.
TEST(GuestMemory, CoverageCountsTheBackedRegions) {
    const Core::MemoryManager::Coverage before = memory().BackingCoverage();
    EXPECT_EQ(before.all_regions, vmem::regions().size());
    EXPECT_EQ(before.covered, before.regions);
    EXPECT_EQ(before.table_pages, before.pages);
    Direct d(3);
    void* const p = flexible(2);
    const Core::MemoryManager::Coverage during = memory().BackingCoverage();
    EXPECT_EQ(during.all_regions, before.all_regions + 2);
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
}

// Unregisters the rasterizer before it goes, also when a test returns early.
struct Unregister {
    ~Unregister() {
        memory().SetRasterizer(nullptr);
    }
};

// The rasterizer hears of the GPU-visible mappings of direct and flexible memory made before it,
// then of each map, unmap and release, and of nothing once it unregistered.
TEST(GuestMemory, TheRasterizerFollowsTheMappings) {
    if (const std::string reason = no_vulkan_reason(); !reason.empty()) {
        GTEST_SKIP() << reason;
    }
    auto liverpool = std::make_unique<AmdGpu::Liverpool>(memory());
    Frontend::WindowSDL window;
    {
        Vulkan::Instance instance{window, -1};
        start_imgui(instance);
        Vulkan::Scheduler scheduler{instance};
        Vulkan::Runtime runtime{instance, scheduler};
        Direct early(1);
        void* const early_flexible = flexible(1);
        void* reservation = nullptr;
        ASSERT_EQ(reserve_range(&reservation, page_size, 0, 0), 0);
        Vulkan::Rasterizer rasterizer{instance, scheduler, runtime, liverpool.get()};
        const Unregister unregister;
        EXPECT_TRUE(rasterizer.IsMapped(addr(early.address), early.length));
        EXPECT_TRUE(rasterizer.IsMapped(addr(early_flexible), page_size));
        EXPECT_FALSE(rasterizer.IsMapped(addr(reservation), page_size));
        early.unmap();
        EXPECT_EQ(sceKernelMunmap(early_flexible, page_size), 0);
        EXPECT_EQ(sceKernelMunmap(reservation, page_size), 0);
        EXPECT_FALSE(rasterizer.IsMapped(addr(early.address), early.length));
        EXPECT_FALSE(rasterizer.IsMapped(addr(early_flexible), page_size));
        // A released range that stays mapped leaves the caches, not the backing.
        Direct released(2);
        const std::uintptr_t kept = addr(released.address);
        EXPECT_TRUE(rasterizer.IsMapped(kept, released.length));
        EXPECT_EQ(release_direct(released.offset + page_size, page_size), 0);
        EXPECT_TRUE(rasterizer.IsMapped(kept, page_size));
        EXPECT_FALSE(rasterizer.IsMapped(kept + page_size, page_size));
        EXPECT_NE(entry(kept + page_size), nullptr);
        EXPECT_EQ(sceKernelMunmap(released.address, released.length), 0);
        EXPECT_EQ(release_direct(released.offset, page_size), 0);
        EXPECT_FALSE(rasterizer.IsMapped(kept, released.length));
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

// The page manager write-protects the pages of an image the texture cache uploaded from guest
// memory. A guest write there faults; the fault handler passes it to the page manager, which
// claims it, and the image is marked as written by the CPU. A read(2) into such a page fails with
// EFAULT, but sceKernelRead lifts the protection first (docs/design/renderer.md, "System calls
// into tracked memory"). A guest mprotect of the pages drops them from the tracking.
TEST(GuestMemory, GuestWritesToTrackedPagesAreClaimed) {
    if (const std::string reason = no_vulkan_reason(); !reason.empty()) {
        GTEST_SKIP() << reason;
    }
    bb::shadps4::install_fault_handler();
    auto liverpool = std::make_unique<AmdGpu::Liverpool>(memory());
    Frontend::WindowSDL window;
    {
        Vulkan::Instance instance{window, -1};
        start_imgui(instance);
        Vulkan::Scheduler scheduler{instance};
        Vulkan::Runtime runtime{instance, scheduler};
        Vulkan::Rasterizer rasterizer{instance, scheduler, runtime, liverpool.get()};
        const Unregister unregister;
        Direct d(2);
        auto* const bytes = static_cast<char*>(d.address);
        // A 64 x 64 linear display buffer at the start of the mapping.
        Libraries::VideoOut::BufferAttributeGroup group{};
        group.is_occupied = true;
        group.attrib.pixel_format = Libraries::VideoOut::PixelFormat::A8R8G8B8Srgb;
        group.attrib.tiling_mode = Libraries::VideoOut::TilingMode::Linear;
        group.attrib.width = group.attrib.height = group.attrib.pitch_in_pixel = 64;
        auto& textures = rasterizer.GetTextureCache();
        VideoCore::TextureCache::ImageDesc desc{group, addr(d.address)};
        const VideoCore::ImageId image = textures.FindImage(desc);
        ASSERT_LE(textures.GetImage(image).info.guest_size, d.length);
        const auto cpu_dirty = [&] {
            return True(textures.GetImage(image).flags & VideoCore::ImageFlagBits::CpuDirty);
        };
        // Uploads the image, after which the page manager tracks writes to its pages.
        textures.UpdateImage(image);
        ASSERT_FALSE(cpu_dirty());

        std::string path =
            (std::filesystem::temp_directory_path() / "bb-tracked-XXXXXX").string();
        const int host = ::mkstemp(path.data());
        ASSERT_GE(host, 0);
        ::unlink(path.c_str());
        ASSERT_EQ(::write(host, "file data", 9), 9);
        // Protected: the host kernel can't write it.
        EXPECT_EQ(::pread(host, bytes + 0x40, 9, 0), -1);
        EXPECT_EQ(errno, EFAULT);

        const std::uint64_t claimed = bb::shadps4::claimed_faults();
        static_cast<volatile char*>(d.address)[0x100] = 'g';
        EXPECT_EQ(bb::shadps4::claimed_faults(), claimed + 1);
        EXPECT_EQ(bytes[0x100], 'g');
        EXPECT_TRUE(cpu_dirty());

        textures.UpdateImage(image);
        ASSERT_FALSE(cpu_dirty());
        ASSERT_EQ(::pread(host, bytes + 0x40, 9, 0), -1);  // protected again
        ASSERT_EQ(::lseek(host, 0, SEEK_SET), 0);
        const std::int32_t fd = bb::hle::kernel::fd_table().insert(host);
        EXPECT_EQ(bb::hle::kernel::sceKernelRead(fd, bytes + 0x40, 9), 9);
        EXPECT_EQ(std::string(bytes + 0x40, 9), "file data");
        EXPECT_TRUE(cpu_dirty());
        EXPECT_EQ(bb::shadps4::claimed_faults(), claimed + 1);  // lifted first, no fault
        bb::hle::kernel::sceKernelClose(fd);

        // A guest mprotect makes the pages writable behind the page manager's back; it drops
        // them from its tracking, so the image is marked as written by the CPU.
        textures.UpdateImage(image);
        ASSERT_FALSE(cpu_dirty());
        ASSERT_EQ(bb::hle::kernel::sceKernelMprotect(d.address, d.length, 3), 0);
        EXPECT_TRUE(cpu_dirty());
        static_cast<volatile char*>(d.address)[0x200] = 'm';
        EXPECT_EQ(bb::shadps4::claimed_faults(), claimed + 1);
        d.unmap();
    }
    liverpool.reset();
}

}  // namespace
