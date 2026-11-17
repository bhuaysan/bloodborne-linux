// The guest memory video_core sees (src/shadps4_host/memory.cpp): the backing is the direct memory
// memfd, its page table follows the kernel's mappings of direct and flexible memory, and the
// rasterizer hears of each GPU-visible map and unmap. Its write tracking takes the faults on the
// pages the caches protected, and the kernel's reads into guest memory lift the protection first.

#include <dlfcn.h>
#include <sys/uio.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <atomic>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#ifdef __SANITIZE_ADDRESS__
#include <sanitizer/lsan_interface.h>
#endif
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
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/buffer_cache/memory_tracker.h"
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
    explicit Direct(std::uint64_t pages, std::uint64_t alignment = 0) : length(pages * page_size) {
        EXPECT_EQ(allocate_direct(0, direct_memory_size(), length, alignment, 0, &offset), 0);
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

// Whether the memfd has a page at `offset`, or a hole there that reads as zeros.
bool has_data(std::uint64_t offset) {
    return ::lseek(dmem::fd(), static_cast<off_t>(offset), SEEK_DATA) == static_cast<off_t>(offset);
}

// Whether [offset, offset + length) of the memfd has data anywhere.
bool any_data(std::uint64_t offset, std::uint64_t length) {
    const off_t data = ::lseek(dmem::fd(), static_cast<off_t>(offset), SEEK_DATA);
    return data >= 0 && static_cast<std::uint64_t>(data) < offset + length;
}

// Shared memory in huge pages (a host's shmem_enabled, or a tmpfs mounted huge=) comes in 2 MiB
// blocks of the memfd: so that a test's memory shares no block with another allocation, it
// allocates whole blocks.
constexpr std::uint64_t huge_page = 2 * 1024 * 1024;

// A fresh allocation is one hole. In CI the memfd's holes have turned into data on their own,
// within 200 ms and also while the test only slept (#549: in the release jobs on Azure and
// AWS kernels, never in the debug job or locally; the cause isn't known), so tests here never rely
// on a hole staying one: a test that starts on holes punches its blocks first, so its memory reads
// zeros either way, and the checks of which parts a large copy reads use CopyBackingWith with a
// hole map of their own (below). Whether [offset, offset + length) is one hole now.
bool make_hole(std::uint64_t offset, std::uint64_t length) {
    if (any_data(offset, length)) {
        std::printf("data in a fresh allocation at %#llx: punched\n",
                    static_cast<unsigned long long>(offset));
        if (::fallocate(dmem::fd(), FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                        static_cast<off_t>(offset), static_cast<off_t>(length)) != 0) {
            return false;
        }
    }
    return !any_data(offset, length);
}

// A large copy (#531) reads the memfd's holes, which the guest never wrote, as zeros without
// faulting pages in for them, and copies the written bytes around them: here across two
// mappings whose backings lie the other way round in the memfd, from and to odd offsets.
TEST(GuestMemory, LargeCopiesSkipTheHolesOfTheBacking) {
    memory();
    constexpr std::uint64_t half = 32 * page_size;  // 512 KiB
    void* range = nullptr;
    ASSERT_EQ(reserve_range(&range, 2 * half, 0, 0), 0);
    std::uint64_t offset = 0;
    // One block of huge pages (aligned to the copy's 64 KiB steps too, which read the holes in
    // a step with data), of which the test uses the first half.
    ASSERT_EQ(allocate_direct(0, direct_memory_size(), huge_page, huge_page, 0, &offset), 0);
    ASSERT_TRUE(make_hole(offset, huge_page));
    const BatchEntry swapped[2] = {
        {addr(range), offset + half, half, 3, {}, dmem::batch_map},
        {addr(range) + half, offset, half, 3, {}, dmem::batch_map},
    };
    ASSERT_EQ(map_direct_batch(swapped, 2, nullptr), 0);
    auto* const guest = static_cast<u8*>(range);
    std::vector<u8> expected(2 * half, 0);
    const auto write = [&](std::uint64_t at, u8 value, std::uint64_t length) {
        std::memset(guest + at, value, length);
        std::memset(expected.data() + at, value, length);
    };
    write(0, 0x11, 3);                                 // the first bytes
    write(page_size - 2, 0x22, 4);                     // across a page boundary
    write(5 * page_size + 100, 0x33, 3 * page_size);   // several pages
    write(half - 1, 0x44, 2);                          // across the two mappings
    write(2 * half - 1, 0x55, 1);                      // the last byte
    const std::uint64_t from = 1, length = 2 * half - 2;
    std::vector<u8> read(length, 0xee);
    memory().CopySparseMemory(addr(range) + from, read.data(), length);
    EXPECT_TRUE(std::equal(read.begin(), read.end(), expected.begin() + from));
    // The written pages are data (the backing of the second half comes first in the memfd).
    EXPECT_TRUE(has_data(offset + half));
    // A copy under the size that looks for holes reads the same.
    std::vector<u8> small(3 * page_size, 0xee);
    memory().CopySparseMemory(addr(range) + 4 * page_size, small.data(), small.size());
    EXPECT_TRUE(std::equal(small.begin(), small.end(), expected.begin() + 4 * page_size));
    EXPECT_EQ(sceKernelMunmap(range, 2 * half), 0);
    EXPECT_EQ(release_direct(offset, huge_page), 0);
}

// A backing for CopyBackingWith: anonymous memory, zeros where nothing is written, whose pages
// in the ranges given to forbid() can't be read, so a copy that reads one ends the test with
// SIGSEGV. With its hole map, the checks of which parts a large copy reads depend on nothing the
// host does to the memfd (#549).
struct HoleMap {
    u8* base = nullptr;
    std::uint64_t size = 0;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> data;  // [start, end), ascending
    int seeks = 0;

    explicit HoleMap(std::uint64_t bytes) : size(bytes) {
        void* const p =
            ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            std::perror("mmap");
            std::abort();
        }
        base = static_cast<u8*>(p);
    }
    ~HoleMap() {
        ::munmap(base, size);
    }
    // Writes `length` bytes of a pattern at `at` into the backing and `expected`, as data.
    void write(std::uint64_t at, std::uint64_t length, std::vector<u8>& expected) {
        for (std::uint64_t i = at; i < at + length; ++i) {
            base[i] = expected[i] = static_cast<u8>(i * 7 + 1);
        }
        data.emplace_back(at - at % page_size, (at + length + page_size - 1) / page_size * page_size);
    }
    void forbid(std::uint64_t start, std::uint64_t end) {
        EXPECT_EQ(::mprotect(base + start, end - start, PROT_NONE), 0);
    }
    // SEEK_DATA over the map: the next data at or after `at`, or past the end.
    s64 next_data(std::uint64_t at) {
        ++seeks;
        for (const auto& [start, end] : data) {
            if (end > at) {
                return static_cast<s64>(std::max(start, at));
            }
        }
        return std::numeric_limits<s64>::max();
    }
    void copy(std::uint64_t from, u8* dest, std::uint64_t length) {
        Core::CopyBackingWith(base + from, from, dest, length,
                              [this](u64 at) { return next_data(at); });
    }
};

// The seek large copies of the memfd use: the next data, past the end where there is none, and
// an error for a bad descriptor.
TEST(GuestMemory, TheBackingSeekFindsDataAndTheEnd) {
    // Whole huge-page blocks, so the data stays one block wherever shmem takes larger folios.
    const int fd = ::memfd_create("bb-seek-test", MFD_CLOEXEC);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(::ftruncate(fd, 3 * huge_page), 0);
    const u8 byte = 1;
    ASSERT_EQ(::pwrite(fd, &byte, 1, huge_page), 1);
    EXPECT_EQ(Core::NextBackingData(fd, 0), static_cast<s64>(huge_page));
    EXPECT_EQ(Core::NextBackingData(fd, huge_page), static_cast<s64>(huge_page));
    EXPECT_EQ(Core::NextBackingData(fd, 2 * huge_page), std::numeric_limits<s64>::max());
    ::close(fd);
    EXPECT_EQ(Core::NextBackingData(-1, 0), -1);
}

// A large copy reads the holes in a step with data and skips the steps without, also the hole
// before the data in a step it starts on, and copies the data around them.
TEST(GuestMemory, LargeCopiesReadOnlyTheStepsWithData) {
    constexpr std::uint64_t size = 1024 * 1024, step = 64 * 1024;
    HoleMap map(size);
    std::vector<u8> expected(size, 0);
    map.write(0, 3, expected);                            // the first bytes, in step 0
    map.write(5 * step + 2 * page_size - 2, 4, expected);  // across a page boundary, in step 5
    map.write(size - page_size + 100, 3, expected);       // in the last page, step 15
    map.forbid(step, 5 * step + page_size);  // up to the data's page in step 5
    map.forbid(6 * step, size - page_size);
    const std::uint64_t from = 1, length = size - 2;
    std::vector<u8> read(length, 0xee);
    map.copy(from, read.data(), length);
    EXPECT_TRUE(std::equal(read.begin(), read.end(), expected.begin() + from));
    EXPECT_EQ(map.seeks, 3);  // at 1, 64 KiB and 384 KiB
    // A copy that ends in a hole: the next data the seek finds lies past its end, and nothing is
    // written after the copy's end.
    const std::uint64_t head = 5 * step;
    std::vector<u8> head_read(head + page_size, 0xee);
    map.copy(0, head_read.data(), head);
    EXPECT_TRUE(std::equal(head_read.begin(), head_read.begin() + head, expected.begin()));
    EXPECT_TRUE(std::all_of(head_read.begin() + head, head_read.end(),
                            [](u8 b) { return b == 0xee; }));
}

// Over a long run of data the large copy's steps grow (fewer seeks), and after it they read at
// most 1 MiB of the hole that follows: a hole further on isn't read, and the data after it is
// copied.
TEST(GuestMemory, LargeCopiesReadBoundedHolesAfterLongData) {
    constexpr std::uint64_t size = 8 * 1024 * 1024;
    constexpr std::uint64_t data = 2560 * 1024;  // 2.5 MiB written from the start
    HoleMap map(size);
    std::vector<u8> expected(size, 0);
    map.write(0, data, expected);
    map.write(size - 3, 1, expected);
    map.forbid(data + 1024 * 1024, size - page_size);
    std::vector<u8> read(size, 0xee);
    map.copy(0, read.data(), size);
    EXPECT_TRUE(std::equal(read.begin(), read.end(), expected.begin()));
    // Steps of 64, 128, 256, 512, 1024 and 1024 KiB, of which the last reads 448 KiB of the hole,
    // and one seek to the last page.
    EXPECT_EQ(map.seeks, 7);
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

// The page of the write tracking, a quarter of the kernel's.
constexpr u64 tp = VideoCore::BYTES_PER_PAGE;
constexpr u64 pages_per_kernel_page = page_size / tp;

// A rasterizer on lavapipe over the guest memory, with the process's fault handler, for the write
// tracking tests below. Members are destroyed in reverse order: the rasterizer before the device,
// Liverpool last.
struct Gpu {
    std::unique_ptr<AmdGpu::Liverpool> liverpool = std::make_unique<AmdGpu::Liverpool>(memory());
    Frontend::WindowSDL window;
    Vulkan::Instance instance{window, -1};
    bool imgui = (start_imgui(instance), true);
    Vulkan::Scheduler scheduler{instance};
    Vulkan::Runtime runtime{instance, scheduler};
    Vulkan::Rasterizer rasterizer{instance, scheduler, runtime, liverpool.get()};
    Unregister unregister;

    Gpu() {
        bb::shadps4::install_fault_handler();
    }
    VideoCore::MemoryTracker& tracker() {
        return rasterizer.GetBufferCache().GetMemoryTracker();
    }
    // What the buffer cache does for a draw that reads [start, start + size): the CPU modified
    // pages in it are write protected again and their ranges returned, to be copied after.
    std::vector<std::pair<u64, u64>> sync(std::uintptr_t start, u64 size) {
        std::vector<std::pair<u64, u64>> ranges;
        tracker().ForEachUploadRange(start, size, false,
                                     [&](u64 at, u64 bytes) { ranges.emplace_back(at, bytes); });
        return ranges;
    }
};

// Whether the page at `p` takes a write without a fault: the host kernel's write into it fails
// on a protected page. It writes back the byte that is there.
bool writable(void* p) {
    char byte = *static_cast<volatile char*>(p);
    const iovec local{&byte, 1};
    const iovec remote{p, 1};
    return ::process_vm_writev(::getpid(), &local, 1, &remote, 1, 0) == 1;
}

// The first page of `pages` pages in `d` that lie in one 16 MiB region of the buffer cache's
// tracking, where a fault's run of pages ends.
char* in_one_region(const Direct& d, u64 pages) {
    auto* const start = static_cast<char*>(d.address);
    const std::uintptr_t region_end = (addr(start) | VideoCore::HIGHER_PAGE_MASK) + 1;
    return addr(start) + pages * tp <= region_end ? start : start + pages * tp;
}

// A write fault on a page the buffer cache watches makes that page and the 15 after it writable
// in one go (#530) and marks them modified by the CPU, so the guest's next writes there take no
// fault and a sync uploads them.
TEST(GuestMemory, AFaultMakesTheNextPagesWritable) {
    if (const std::string reason = no_vulkan_reason(); !reason.empty()) {
        GTEST_SKIP() << reason;
    }
    {
        Gpu gpu;
        Direct d(64 / pages_per_kernel_page);
        char* const base = in_one_region(d, 32);
        gpu.sync(addr(base), 32 * tp);
        for (u64 page = 0; page < 32; ++page) {
            ASSERT_FALSE(writable(base + page * tp)) << page;
        }
        EXPECT_FALSE(gpu.tracker().IsRegionCpuModified(addr(base), 32 * tp));

        const std::uint64_t claimed = bb::shadps4::claimed_faults();
        static_cast<volatile char*>(base)[8] = 'a';
        EXPECT_EQ(bb::shadps4::claimed_faults(), claimed + 1);
        for (u64 page = 0; page < 16; ++page) {
            EXPECT_TRUE(writable(base + page * tp)) << page;
            EXPECT_TRUE(gpu.tracker().IsRegionCpuModified(addr(base) + page * tp, 1));
        }
        for (u64 page = 16; page < 32; ++page) {
            EXPECT_FALSE(writable(base + page * tp)) << page;
            EXPECT_FALSE(gpu.tracker().IsRegionCpuModified(addr(base) + page * tp, 1));
        }
        for (u64 page = 1; page < 16; ++page) {
            static_cast<volatile char*>(base)[page * tp] = 'b';
        }
        EXPECT_EQ(bb::shadps4::claimed_faults(), claimed + 1);

        // The sync uploads the 16 pages as one range and protects them again.
        const auto ranges = gpu.sync(addr(base), 32 * tp);
        ASSERT_EQ(ranges.size(), 1u);
        EXPECT_EQ(ranges[0].first, addr(base));
        EXPECT_EQ(ranges[0].second, 16 * tp);
        for (u64 page = 0; page < 32; ++page) {
            EXPECT_FALSE(writable(base + page * tp)) << page;
        }

        // A fault near the end of the tracked range takes only what is left of it: pages the
        // buffer cache does not watch (CPU modified already) end the run.
        gpu.sync(addr(base), 32 * tp);
        static_cast<volatile char*>(base)[28 * tp] = 'c';
        EXPECT_EQ(bb::shadps4::claimed_faults(), claimed + 2);
        for (u64 page = 28; page < 32; ++page) {
            EXPECT_TRUE(writable(base + page * tp)) << page;
        }
        EXPECT_FALSE(writable(base + 27 * tp));
        d.unmap();
    }
}

// The run of pages a fault makes writable ends before a page the GPU wrote (an upload of guest
// memory would overwrite what it wrote) and before a page an image of the texture cache watches
// too (that page's fault has to reach the image).
TEST(GuestMemory, AFaultsRunEndsAtGpuWrittenAndImagePages) {
    if (const std::string reason = no_vulkan_reason(); !reason.empty()) {
        GTEST_SKIP() << reason;
    }
    {
        Gpu gpu;
        Direct d(64 / pages_per_kernel_page);
        char* const base = in_one_region(d, 32);
        gpu.sync(addr(base), 32 * tp);
        gpu.tracker().MarkRegionAsGpuModified(addr(base) + 4 * tp, tp);
        const std::uint64_t claimed = bb::shadps4::claimed_faults();
        static_cast<volatile char*>(base)[0] = 'a';
        EXPECT_EQ(bb::shadps4::claimed_faults(), claimed + 1);
        for (u64 page = 0; page < 4; ++page) {
            EXPECT_TRUE(writable(base + page * tp)) << page;
        }
        EXPECT_FALSE(writable(base + 4 * tp));
        EXPECT_FALSE(gpu.tracker().IsRegionCpuModified(addr(base) + 4 * tp, tp));

        // A 64 x 64 linear image over pages 16 to 19 (16 KiB).
        Libraries::VideoOut::BufferAttributeGroup group{};
        group.is_occupied = true;
        group.attrib.pixel_format = Libraries::VideoOut::PixelFormat::A8R8G8B8Srgb;
        group.attrib.tiling_mode = Libraries::VideoOut::TilingMode::Linear;
        group.attrib.width = group.attrib.height = group.attrib.pitch_in_pixel = 64;
        auto& textures = gpu.rasterizer.GetTextureCache();
        char* const image_at = base + 16 * tp;
        VideoCore::TextureCache::ImageDesc desc{group, addr(image_at)};
        const VideoCore::ImageId image = textures.FindImage(desc);
        ASSERT_EQ(textures.GetImage(image).info.guest_size, 4 * tp);
        textures.UpdateImage(image);
        const auto cpu_dirty = [&] {
            return True(textures.GetImage(image).flags & VideoCore::ImageFlagBits::CpuDirty);
        };
        ASSERT_FALSE(cpu_dirty());
        static_cast<volatile char*>(base)[10 * tp] = 'b';
        EXPECT_EQ(bb::shadps4::claimed_faults(), claimed + 2);
        for (u64 page = 10; page < 16; ++page) {
            EXPECT_TRUE(writable(base + page * tp)) << page;
        }
        EXPECT_FALSE(writable(image_at));
        EXPECT_FALSE(cpu_dirty());
        static_cast<volatile char*>(image_at)[tp + 4] = 'c';
        EXPECT_EQ(bb::shadps4::claimed_faults(), claimed + 3);
        EXPECT_TRUE(cpu_dirty());
        EXPECT_EQ(image_at[tp + 4], 'c');
        d.unmap();
    }
}

// FindImage remembers a perfect match per request (#590). A repeated request returns the same
// image; once that image leaves the cache, the next request walks the page table again and gets a
// registered image, never the freed one. A request that differs only in `exact_fmt` is its own.
TEST(GuestMemory, FindImageForgetsAFreedImage) {
    if (const std::string reason = no_vulkan_reason(); !reason.empty()) {
        GTEST_SKIP() << reason;
    }
    {
        Gpu gpu;
        Direct d(1);
        Libraries::VideoOut::BufferAttributeGroup group{};
        group.is_occupied = true;
        group.attrib.pixel_format = Libraries::VideoOut::PixelFormat::A8R8G8B8Srgb;
        group.attrib.tiling_mode = Libraries::VideoOut::TilingMode::Linear;
        group.attrib.width = group.attrib.height = group.attrib.pitch_in_pixel = 64;
        auto& textures = gpu.rasterizer.GetTextureCache();
        const auto find = [&](bool exact_fmt = false) {
            VideoCore::TextureCache::ImageDesc desc{group, addr(d.address)};
            return textures.FindImage(desc, exact_fmt);
        };
        const auto registered = [&](VideoCore::ImageId id) {
            return True(textures.GetImage(id).flags & VideoCore::ImageFlagBits::Registered);
        };
        const VideoCore::ImageId first = find();
        ASSERT_TRUE(registered(first));
        EXPECT_EQ(find(), first);
        EXPECT_EQ(find(true), first);
        EXPECT_EQ(find(), first);

        // Frees the image; its slot stays allocated until the scheduler's deferred deletion.
        textures.UnmapMemory(addr(d.address), d.length);
        const VideoCore::ImageId second = find();
        EXPECT_NE(second, first);
        EXPECT_TRUE(registered(second));
        EXPECT_EQ(find(true), second);
        EXPECT_EQ(find(), second);
        d.unmap();
    }
}

// A remembered match skips stamping and touching its image again within one scheduler tick and
// GC tick, and does both once either tick moves on, as a full FindImage would.
TEST(GuestMemory, FindImageStampsItsImageOnceATickMovesOn) {
    if (const std::string reason = no_vulkan_reason(); !reason.empty()) {
        GTEST_SKIP() << reason;
    }
    {
        Gpu gpu;
        Direct d(1);
        Libraries::VideoOut::BufferAttributeGroup group{};
        group.is_occupied = true;
        group.attrib.pixel_format = Libraries::VideoOut::PixelFormat::A8R8G8B8Srgb;
        group.attrib.tiling_mode = Libraries::VideoOut::TilingMode::Linear;
        group.attrib.width = group.attrib.height = group.attrib.pitch_in_pixel = 64;
        auto& textures = gpu.rasterizer.GetTextureCache();
        const auto find = [&] {
            VideoCore::TextureCache::ImageDesc desc{group, addr(d.address)};
            return textures.FindImage(desc);
        };
        const VideoCore::ImageId image = find();
        EXPECT_EQ(find(), image);
        EXPECT_EQ(textures.GetImage(image).tick_accessed_last, gpu.scheduler.CurrentTick());

        const u64 before = gpu.scheduler.CurrentTick();
        gpu.scheduler.Flush();
        ASSERT_GT(gpu.scheduler.CurrentTick(), before);
        EXPECT_EQ(find(), image);
        EXPECT_EQ(textures.GetImage(image).tick_accessed_last, gpu.scheduler.CurrentTick());

        // A new GC tick alone: the touch has to reach the image again.
        textures.RunGarbageCollector();
        textures.GetImage(image).tick_accessed_last = 0;  // shows whether the next hit stamps
        EXPECT_EQ(find(), image);
        EXPECT_EQ(textures.GetImage(image).tick_accessed_last, gpu.scheduler.CurrentTick());
        d.unmap();
    }
}

// The buffer cache never frees the device memory that makes its sparse blocks resident (upstream
// keeps it to the end), which LeakSanitizer reports. Allocations made while one of these lives are
// left out of its report.
struct NoLeakCheck {
#ifdef __SANITIZE_ADDRESS__
    NoLeakCheck() {
        __lsan_disable();
    }
    ~NoLeakCheck() {
        __lsan_enable();
    }
    NoLeakCheck(const NoLeakCheck&) = delete;
    NoLeakCheck& operator=(const NoLeakCheck&) = delete;
#endif
};

// What the GPU wrote on a page reaches guest memory before the CPU writes elsewhere on that page,
// so the upload after the write keeps it (#537: a vertex buffer the GPU wrote lost its vertices
// that way after a respawn). That holds for a guest write that faults, for a system call that
// writes into the page and for a guest mprotect of it.
TEST(GuestMemory, ACpuWriteKeepsWhatTheGpuWroteOnThePage) {
    if (const std::string reason = no_vulkan_reason(); !reason.empty()) {
        GTEST_SKIP() << reason;
    }
    {
        Gpu gpu;
        Direct d(1);
        auto* const base = static_cast<char*>(d.address);
        // The buffer cache keeps its copy in a sparse buffer of 4 GiB, which lavapipe can't
        // create (out of device memory), so the test runs only on a GPU that can, such as with
        // VK_LOADER_DRIVERS_SELECT='*radeon*'.
        try {
            [[maybe_unused]] const NoLeakCheck no_leak_check;
            static_cast<void>(gpu.rasterizer.GetBufferCache().ObtainBuffer(
                addr(base), 64, VideoCore::ObtainBufferFlags::IgnoreStreamBuffer));
        } catch (const std::exception& e) {
            if (std::string(e.what()).find("ErrorOutOfDeviceMemory") == std::string::npos) {
                throw;
            }
            d.unmap();
            GTEST_SKIP() << "no sparse buffer of the buffer cache on this device: " << e.what();
        }
        // The GPU fills the first 64 bytes of the page at `page` with `fill`, after the guest
        // filled the page with 'o'. Guest memory keeps the 'o's.
        const auto gpu_fill = [&](u64 page, char fill) {
            char* const at = base + page * tp;
            std::memset(at, 'o', tp);
            const VAddr start = addr(at);
            [[maybe_unused]] const NoLeakCheck no_leak_check;
            static_cast<void>(gpu.rasterizer.GetBufferCache().ObtainBuffer(
                start, 64, VideoCore::ObtainBufferFlags::IsWritten));
            u32 value;
            std::memset(&value, fill, sizeof(value));
            gpu.rasterizer.FillBuffer(start, 64, value, false);
            gpu.rasterizer.Finish();
            ASSERT_TRUE(gpu.tracker().IsRegionGpuModified(start, tp));
            ASSERT_EQ(std::string(at, 64), std::string(64, 'o'));
        };

        gpu_fill(0, 'G');
        const std::uint64_t claimed = bb::shadps4::claimed_faults();
        static_cast<volatile char*>(base)[tp - 1] = 'c';
        EXPECT_EQ(bb::shadps4::claimed_faults(), claimed + 1);
        EXPECT_EQ(std::string(base, 64), std::string(64, 'G'));
        EXPECT_EQ(base[tp - 1], 'c');
        EXPECT_FALSE(gpu.tracker().IsRegionGpuModified(addr(base), tp));
        EXPECT_TRUE(gpu.tracker().IsRegionCpuModified(addr(base), tp));

        gpu_fill(1, 'H');
        std::string path =
            (std::filesystem::temp_directory_path() / "bb-gpu-page-XXXXXX").string();
        const int host = ::mkstemp(path.data());
        ASSERT_GE(host, 0);
        ::unlink(path.c_str());
        ASSERT_EQ(::write(host, "file data", 9), 9);
        ASSERT_EQ(::lseek(host, 0, SEEK_SET), 0);
        const std::int32_t fd = bb::hle::kernel::fd_table().insert(host);
        char* const page1 = base + tp;
        EXPECT_EQ(bb::hle::kernel::sceKernelRead(fd, page1 + 0x800, 9), 9);
        bb::hle::kernel::sceKernelClose(fd);
        EXPECT_EQ(std::string(page1, 64), std::string(64, 'H'));
        EXPECT_EQ(std::string(page1 + 0x800, 9), "file data");

        gpu_fill(2, 'I');
        ASSERT_EQ(bb::hle::kernel::sceKernelMprotect(d.address, d.length, 3), 0);
        EXPECT_EQ(std::string(base + 2 * tp, 64), std::string(64, 'I'));

        // An unmap downloads nothing but drops the GPU state of the range, so that no later
        // download writes the old bytes over what is mapped there next.
        gpu_fill(3, 'J');
        d.unmap();
        EXPECT_FALSE(gpu.tracker().IsRegionGpuModified(addr(base), d.length));
    }
}

// Write tracking under load: guest threads write runs of pages, each write to a protected page a
// fault that marks pages modified by the CPU and unprotects them, while the GPU thread syncs the
// range (protecting the modified pages again and copying them into its copy of the memory) and
// keeps an image over part of it tracked. A write the tracking missed would leave the copy stale:
// once the writers stop, one more sync has to make it equal to guest memory, with every page
// protected again. With `through_backing` the copy is CopySparseMemory's, as the buffer cache's:
// the memory starts as one hole of the memfd, and the writers' first writes to its pages race
// with the copies that skip the holes (#531).
void concurrent_faults_and_syncs(bool through_backing) {
    {
        Gpu gpu;
        constexpr u64 pages = 512;  // 2 MiB, 8 words of the region bits
        constexpr u64 words_per_page = tp / sizeof(u64);
        static_assert(pages * tp == huge_page);  // one block of huge pages
        Direct d(pages / pages_per_kernel_page, huge_page);
        if (through_backing) {
            ASSERT_TRUE(make_hole(d.offset, d.length));  // the copies start on one hole
        }
        auto* const words = static_cast<u64*>(d.address);
        const std::uintptr_t start = addr(d.address);
        const u64 size = pages * tp;
        std::vector<u64> copy(pages * words_per_page);
        const auto upload = [&](const std::vector<std::pair<u64, u64>>& ranges) {
            for (const auto& [at, bytes] : ranges) {
                const u64 first = (at - start) / sizeof(u64);
                if (through_backing) {
                    memory().CopySparseMemory(at, reinterpret_cast<u8*>(&copy[first]), bytes);
                    continue;
                }
                for (u64 i = first; i < first + bytes / sizeof(u64); ++i) {
                    copy[i] = std::atomic_ref<u64>(words[i]).load(std::memory_order_relaxed);
                }
            }
        };
        upload(gpu.sync(start, size));

        Libraries::VideoOut::BufferAttributeGroup group{};
        group.is_occupied = true;
        group.attrib.pixel_format = Libraries::VideoOut::PixelFormat::A8R8G8B8Srgb;
        group.attrib.tiling_mode = Libraries::VideoOut::TilingMode::Linear;
        group.attrib.width = group.attrib.height = group.attrib.pitch_in_pixel = 64;
        auto& textures = gpu.rasterizer.GetTextureCache();
        VideoCore::TextureCache::ImageDesc desc{group, start + 100 * tp};
        const VideoCore::ImageId image = textures.FindImage(desc);

        const std::uint64_t claimed = bb::shadps4::claimed_faults();
        constexpr int writers = 4;
        std::atomic<bool> stop{false};
        std::vector<std::thread> threads;
        for (int t = 0; t < writers; ++t) {
            threads.emplace_back([&, t] {
                std::mt19937_64 random(t + 1);
                for (u64 run = 0; !stop.load(std::memory_order_relaxed); ++run) {
                    const u64 first = random() % pages;
                    const u64 length = std::min<u64>(1 + random() % 24, pages - first);
                    for (u64 page = first; page < first + length; ++page) {
                        const u64 at = page * words_per_page + random() % words_per_page;
                        std::atomic_ref<u64>(words[at]).store(
                            (u64(t) << 56) | (run << 16) | page, std::memory_order_relaxed);
                    }
                }
            });
        }
        // Every other sync takes the whole range, the others a random part of it. The syncs go
        // on until the writers have faulted often enough: an optimized build can finish a fixed
        // number of syncs before the writer threads write anything, which would test nothing.
        std::mt19937_64 random(99);
        constexpr u64 min_syncs = 500;
        constexpr u64 min_faults = 1000;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        u64 sync = 0;
        for (; sync < min_syncs || bb::shadps4::claimed_faults() - claimed <= min_faults;
             ++sync) {
            if (std::chrono::steady_clock::now() > deadline) {
                break;
            }
            const u64 first = sync % 2 == 0 ? 0 : random() % pages;
            const u64 length =
                sync % 2 == 0 ? pages : std::min<u64>(1 + random() % 64, pages - first);
            upload(gpu.sync(start + first * tp, length * tp));
            if (sync % 64 == 0) {
                textures.UpdateImage(image);
            }
        }
        stop.store(true);
        for (auto& thread : threads) {
            thread.join();
        }
        // Each page the tracking holds unmodified has to match its copy already: a copy that
        // lost bytes (a hole read where the data was) would otherwise go unseen when the
        // writers wrote the page again before the end, as they write the whole range many times.
        u64 stale_clean = 0;
        for (u64 page = 0; page < pages; ++page) {
            if (gpu.tracker().IsRegionCpuModified(start + page * tp, tp)) {
                continue;
            }
            for (u64 i = page * words_per_page; i < (page + 1) * words_per_page; ++i) {
                stale_clean += copy[i] != words[i] ? 1 : 0;
            }
        }
        EXPECT_EQ(stale_clean, 0u);
        upload(gpu.sync(start, size));
        u64 stale = 0;
        for (u64 i = 0; i < copy.size(); ++i) {
            stale += copy[i] != words[i] ? 1 : 0;
        }
        EXPECT_EQ(stale, 0u);
        for (u64 page = 0; page < pages; ++page) {
            EXPECT_FALSE(writable(&words[page * words_per_page])) << page;
        }
        EXPECT_GE(sync, min_syncs);
        EXPECT_GT(bb::shadps4::claimed_faults() - claimed, min_faults) << sync << " syncs";
        d.unmap();
    }
}

TEST(GuestMemory, ConcurrentFaultsAndSyncsMissNoWrite) {
    if (const std::string reason = no_vulkan_reason(); !reason.empty()) {
        GTEST_SKIP() << reason;
    }
    concurrent_faults_and_syncs(false);
}

TEST(GuestMemory, ConcurrentWritesIntoHolesReachTheCopy) {
    if (const std::string reason = no_vulkan_reason(); !reason.empty()) {
        GTEST_SKIP() << reason;
    }
    concurrent_faults_and_syncs(true);
}

}  // namespace
