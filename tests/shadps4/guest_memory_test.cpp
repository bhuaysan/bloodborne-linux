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
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <atomic>
#include <random>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#ifdef __SANITIZE_ADDRESS__
#include <sanitizer/lsan_interface.h>
#endif
#include <imgui.h>
#include <xxhash.h>

#include "core/libraries/videoout/buffer.h"
#include "core/memory.h"
#include "hle/kernel/dmem.hpp"
#include "hle/kernel/file.hpp"
#include "hle/kernel/vmem.hpp"
#include "imgui/renderer/imgui_impl_vulkan.h"
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
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
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
    Vulkan::ValidationOptions validation{};
    std::unique_ptr<AmdGpu::Liverpool> liverpool = std::make_unique<AmdGpu::Liverpool>(memory());
    Frontend::WindowSDL window;
    Vulkan::Instance instance{window, -1, validation};
    bool imgui = (start_imgui(instance), true);
    Vulkan::Scheduler scheduler{instance};
    Vulkan::Runtime runtime{instance, scheduler};
    Vulkan::Rasterizer rasterizer{instance, scheduler, runtime, liverpool.get()};
    Unregister unregister;

    Gpu() {
        bb::shadps4::install_fault_handler();
    }
    explicit Gpu(Vulkan::ValidationOptions options) : validation{options} {
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
        // The collector frees images last touched at or before gc_tick - min(16, gc_tick) once
        // the device's memory use passes its trigger, which depends on what else holds the GPU.
        // At GC tick 0 that is every image; at tick 1, before the image exists, it is none (#600).
        textures.RunGarbageCollector();
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

// MarkAsMaybeGpuDirty notes that the GPU wrote a buffer over an image. Upstream hashes the image's
// guest memory right then, for the collector's choice between downloading the image and dropping
// it. Ours puts the hash off while every page of the image is write protected (#591) and takes it
// before the memory can change, so it is the hash of what upstream hashed: before a guest write
// into the image lands, before a write through the backing, and before a guest write to a page
// the image shares with other data. An upload drops a hash that was put off, which is every
// frame's case in the game, and an image with an unprotected page is hashed at once.
TEST(GuestMemory, AnImagesGpuHashIsTakenBeforeItsMemoryChanges) {
    if (const std::string reason = no_vulkan_reason(); !reason.empty()) {
        GTEST_SKIP() << reason;
    }
    {
        Gpu gpu;
        Direct d(64 / pages_per_kernel_page);
        auto* const base = static_cast<char*>(d.address);
        // A 64 x 64 linear image (16 KiB) from the middle of page 4 to the middle of page 8.
        Libraries::VideoOut::BufferAttributeGroup group{};
        group.is_occupied = true;
        group.attrib.pixel_format = Libraries::VideoOut::PixelFormat::A8R8G8B8Srgb;
        group.attrib.tiling_mode = Libraries::VideoOut::TilingMode::Linear;
        group.attrib.width = group.attrib.height = group.attrib.pitch_in_pixel = 64;
        auto& textures = gpu.rasterizer.GetTextureCache();
        char* const image_at = base + 4 * tp + tp / 2;
        constexpr u64 image_size = 4 * tp;
        for (u64 i = 0; i < image_size; ++i) {
            image_at[i] = static_cast<char>(i * 7);
        }
        VideoCore::TextureCache::ImageDesc desc{group, addr(image_at)};
        const VideoCore::ImageId id = textures.FindImage(desc);
        VideoCore::Image& image = textures.GetImage(id);
        ASSERT_EQ(image.info.guest_size, image_size);
        const auto hash = [&] { return XXH3_64bits(image_at, image_size); };
        const auto gpu_writes_buffer = [&] {
            textures.MarkAsMaybeGpuDirty(addr(image_at), image_size);
        };
        // What a frame does after a shader wrote the buffer: the image is uploaded again.
        const auto upload_again = [&] {
            textures.InvalidateMemoryFromGPU(addr(image_at), image_size);
            textures.UpdateImage(id);
        };
        const auto cpu_dirty = [&] {
            return True(image.flags & VideoCore::ImageFlagBits::CpuDirty);
        };
        textures.UpdateImage(id);
        ASSERT_FALSE(writable(image_at));
        ASSERT_FALSE(writable(image_at + image_size - 1));

        // Put off, and dropped by the upload.
        gpu_writes_buffer();
        EXPECT_TRUE(image.gpu_hash_pending);
        EXPECT_EQ(image.gpu_hash, 0u);
        upload_again();
        EXPECT_FALSE(image.gpu_hash_pending);
        EXPECT_EQ(image.gpu_hash, 0u);

        // A guest write into the image: the hash is of the bytes before it.
        gpu_writes_buffer();
        u64 before = hash();
        const std::uint64_t claimed = bb::shadps4::claimed_faults();
        static_cast<volatile char*>(image_at)[tp] = 'c';
        EXPECT_EQ(bb::shadps4::claimed_faults(), claimed + 1);
        EXPECT_FALSE(image.gpu_hash_pending);
        EXPECT_EQ(image.gpu_hash, before);
        EXPECT_NE(hash(), before);
        EXPECT_TRUE(cpu_dirty());
        // A second buffer write keeps the first one's hash, as upstream.
        gpu_writes_buffer();
        EXPECT_FALSE(image.gpu_hash_pending);
        EXPECT_EQ(image.gpu_hash, before);

        // A write through the backing takes no fault. One beside the image leaves the hash put
        // off; one into the image takes it first.
        textures.UpdateImage(id);
        ASSERT_FALSE(cpu_dirty());
        ASSERT_EQ(image.gpu_hash, 0u);
        gpu_writes_buffer();
        ASSERT_TRUE(image.gpu_hash_pending);
        EXPECT_TRUE(memory().TryWriteBacking(base + 16 * tp, "beside", 6));
        EXPECT_TRUE(image.gpu_hash_pending);
        before = hash();
        EXPECT_TRUE(memory().TryWriteBacking(image_at + 2 * tp, "backing", 7));
        EXPECT_FALSE(image.gpu_hash_pending);
        EXPECT_EQ(image.gpu_hash, before);
        EXPECT_NE(hash(), before);
        EXPECT_FALSE(cpu_dirty());

        // A guest write to the first page before the image starts: the image is not modified,
        // but that page is no longer protected, so the hash is taken.
        upload_again();
        ASSERT_EQ(image.gpu_hash, 0u);
        gpu_writes_buffer();
        ASSERT_TRUE(image.gpu_hash_pending);
        before = hash();
        static_cast<volatile char*>(base)[4 * tp] = 'h';
        EXPECT_EQ(bb::shadps4::claimed_faults(), claimed + 2);
        EXPECT_FALSE(image.gpu_hash_pending);
        EXPECT_EQ(image.gpu_hash, before);
        EXPECT_FALSE(cpu_dirty());
        EXPECT_TRUE(writable(image_at));

        // With that page unprotected a guest write would go unseen: hashed at once. RefreshImage
        // uploads without protecting the pages again.
        textures.InvalidateMemoryFromGPU(addr(image_at), image_size);
        textures.RefreshImage(image);
        ASSERT_EQ(image.gpu_hash, 0u);
        ASSERT_TRUE(writable(image_at));
        image_at[8] = 'u';
        gpu_writes_buffer();
        EXPECT_FALSE(image.gpu_hash_pending);
        EXPECT_EQ(image.gpu_hash, hash());
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

// A guest write during a copy job (#777). The buffer cache protects the CPU modified pages again
// and hands their memmove into staging space to the scheduler's copy threads. A write that comes
// before the job runs faults and marks its page modified again, so the next sync uploads it once
// more; the copy recorded for the draw waits for the job.
TEST(GuestMemory, ACpuWriteDuringACopyIsSyncedAgain) {
    if (const std::string reason = no_vulkan_reason(); !reason.empty()) {
        GTEST_SKIP() << reason;
    }
    {
        Gpu gpu;
        constexpr u64 pages = 16; // one fault's run, 64 KiB: a copy job, not an inline copy
        Direct d(2 * pages / pages_per_kernel_page);
        char* const base = in_one_region(d, pages);
        const std::uintptr_t start = addr(base);
        constexpr u64 size = pages * tp;
        auto& buffers = gpu.rasterizer.GetBufferCache();
        std::memset(base, 'a', size);
        std::pair<const VideoCore::Buffer*, u64> arena;
        // As above: the buffer cache's sparse buffer needs a GPU that can create it.
        try {
            [[maybe_unused]] const NoLeakCheck no_leak_check;
            arena = buffers.ObtainBuffer(start, size,
                                         VideoCore::ObtainBufferFlags::IgnoreStreamBuffer);
        } catch (const std::exception& e) {
            if (std::string(e.what()).find("ErrorOutOfDeviceMemory") == std::string::npos) {
                throw;
            }
            d.unmap();
            GTEST_SKIP() << "no sparse buffer of the buffer cache on this device: " << e.what();
        }
        gpu.rasterizer.Finish();
        ASSERT_FALSE(gpu.tracker().IsRegionCpuModified(start, size));
        std::memset(base, 'b', size); // one fault, then the run is writable

        // Both copy threads wait, so the sync's job stays queued.
        auto& copies = gpu.scheduler.Copies();
        std::atomic<bool> release{false};
        std::atomic<int> waiting{0};
        for (int i = 0; i < 2; ++i) {
            copies.Push([&] {
                ++waiting;
                while (!release.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            });
        }
        while (waiting.load() < 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const u64 before = copies.Issued();
        {
            [[maybe_unused]] const NoLeakCheck no_leak_check;
            arena = buffers.ObtainBuffer(start, size,
                                         VideoCore::ObtainBufferFlags::IgnoreStreamBuffer);
        }
        EXPECT_EQ(copies.Issued(), before + 1);
        EXPECT_LT(copies.Completed(), copies.Issued());
        EXPECT_FALSE(gpu.tracker().IsRegionCpuModified(start, size));
        EXPECT_FALSE(writable(base));

        const std::uint64_t claimed = bb::shadps4::claimed_faults();
        static_cast<volatile char*>(base)[5] = 'c';
        EXPECT_EQ(bb::shadps4::claimed_faults(), claimed + 1);
        EXPECT_TRUE(gpu.tracker().IsRegionCpuModified(start, tp));
        release = true;

        // The submit of the draw's copy from staging waits for the job, which ran after the write.
        auto& staging_pool = gpu.runtime.GetStagingPool();
        const auto readback = staging_pool.Request(size, VideoCore::MemoryType::HostCached);
        auto cmdbuf = gpu.scheduler.CommandBuffer();
        const vk::MemoryBarrier2 to_transfer{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eMemoryRead};
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &to_transfer});
        cmdbuf.copyBuffer(arena.first->Handle(), readback.buffer->Handle(),
                          vk::BufferCopy{arena.second, readback.offset, size});
        const vk::MemoryBarrier2 to_host{.srcStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
                                         .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                                         .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                                         .dstAccessMask = vk::AccessFlagBits2::eHostRead};
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &to_host});
        gpu.rasterizer.Finish();
        readback.Invalidate();
        EXPECT_EQ(copies.Completed(), copies.Issued());
        const auto* const copied = reinterpret_cast<const char*>(readback.mapped);
        EXPECT_EQ(copied[0], 'b');
        EXPECT_EQ(copied[5], 'c');
        EXPECT_EQ(copied[size - 1], 'b');

        // The written page is synced again, and then nothing is left.
        EXPECT_TRUE(gpu.tracker().IsRegionCpuModified(start, tp));
        {
            [[maybe_unused]] const NoLeakCheck no_leak_check;
            static_cast<void>(buffers.ObtainBuffer(
                start, size, VideoCore::ObtainBufferFlags::IgnoreStreamBuffer));
        }
        EXPECT_FALSE(gpu.tracker().IsRegionCpuModified(start, size));
        gpu.rasterizer.Finish();
        d.unmap();
    }
}

// The command processor writes an EOP label only once the copy jobs of earlier buffer syncs are
// done (#777): a game that sees the label may write into a buffer a draw before it read, and that
// draw's copy must have read the buffer first. The test holds both copy threads, so a job pushed
// before the submit stays pending, and the label stays unwritten until the test lets go.
TEST(GuestMemory, ALabelWaitsForEarlierCopies) {
    if (const std::string reason = no_vulkan_reason(); !reason.empty()) {
        GTEST_SKIP() << reason;
    }
    {
        Gpu gpu;
        Direct d(1);
        auto* const dwords = static_cast<std::uint32_t*>(d.address);
        auto& copies = gpu.scheduler.Copies();
        std::atomic<bool> release{false};
        std::atomic<int> waiting{0};
        std::atomic<bool> copied{false};
        for (int i = 0; i < 2; ++i) {
            copies.Push([&] {
                ++waiting;
                while (!release.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            });
        }
        copies.Push([&] { copied = true; });
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (waiting.load() < 2 && std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (waiting.load() < 2) {
            release = true; // the blockers capture locals that go before the rasterizer
            copies.WaitIdle();
            d.unmap();
            FAIL() << "the copy threads did not start";
        }

        // EVENT_WRITE_EOP: write 0x77 to the label, no interrupt.
        const std::uint64_t label = addr(dwords) + 0x800;
        const std::array<std::uint32_t, 6> eop{3u << 30 | 4u << 16 | 0x47u << 8,
                                               0x504,
                                               static_cast<std::uint32_t>(label),
                                               1u << 29 | static_cast<std::uint32_t>(label >> 32),
                                               0x77,
                                               0};
        gpu.liverpool->SubmitGfx(eop, {});
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        EXPECT_EQ(std::atomic_ref(dwords[0x200]).load(), 0u);
        EXPECT_FALSE(gpu.liverpool->IsGpuIdle());
        release = true;
        const auto label_until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::atomic_ref(dwords[0x200]).load() != 0x77u &&
               std::chrono::steady_clock::now() < label_until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        EXPECT_EQ(std::atomic_ref(dwords[0x200]).load(), 0x77u);
        EXPECT_TRUE(copied.load());
        gpu.liverpool->WaitGpuIdle();
        d.unmap();
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
            // As a GPU write into a buffer over the image: its hash is put off on this thread
            // and taken by the writers' faults (#591).
            textures.MarkAsMaybeGpuDirty(start + 100 * tp, 4 * tp);
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

// The texture cache belongs to the GPU thread (#629). Guest threads map blocks of memory, write
// them and unmap them, while the GPU thread finds images in the mapped blocks, tracks and uploads
// them and runs the collector. A guest write to a tracked page is a fault that walks the cache's
// page table on the guest thread, and an unmap frees the images of its block: upstream on the
// guest thread, under the GPU thread's hands (the page table's buckets, the LRU list, the lookup
// table of FindImage). Here the GPU thread frees them between two of its steps. In the end every
// block is unmapped and no image is left where the blocks were.
TEST(GuestMemory, GuestUnmapsFreeImagesOnTheGpuThread) {
    if (const std::string reason = no_vulkan_reason(); !reason.empty()) {
        GTEST_SKIP() << reason;
    }
    {
        Gpu gpu;
        auto& textures = gpu.rasterizer.GetTextureCache();
        Libraries::VideoOut::BufferAttributeGroup group{};
        group.is_occupied = true;
        group.attrib.pixel_format = Libraries::VideoOut::PixelFormat::A8R8G8B8Srgb;
        group.attrib.tiling_mode = Libraries::VideoOut::TilingMode::Linear;
        group.attrib.width = group.attrib.height = group.attrib.pitch_in_pixel = 64;
        constexpr u64 image_size = 64 * 64 * 4;
        constexpr u64 block_size = 4 * page_size;  // 64 KiB: blocks share the table's 256 KiB pages
        constexpr u64 block_images = block_size / image_size;
        constexpr u64 block_words = block_size / sizeof(u64);

        // The block a guest thread has mapped, zero between two blocks, and how often the GPU
        // thread used it. The mutex keeps a block mapped during a step of the GPU thread, as a
        // game unmaps memory only once it no longer draws from it.
        struct Block {
            std::mutex mutex;
            std::uintptr_t address = 0;
            std::atomic<u32> uses{0};
        };
        constexpr int guests = 6;
        std::array<Block, guests> blocks;
        std::atomic<bool> stop{false};
        std::atomic<u64> unmaps{0};
        std::atomic<std::uintptr_t> lowest{std::numeric_limits<std::uintptr_t>::max()};
        std::atomic<std::uintptr_t> highest{0};

        const std::uint64_t claimed = bb::shadps4::claimed_faults();
        std::vector<std::thread> threads;
        for (int t = 0; t < guests; ++t) {
            threads.emplace_back([&, t] {
                std::mt19937_64 random(t + 1);
                Block& block = blocks[t];
                while (!stop.load(std::memory_order_relaxed)) {
                    Direct d(block_size / page_size);
                    const std::uintptr_t start = addr(d.address);
                    for (std::uintptr_t low = lowest.load(); start < low;) {
                        if (lowest.compare_exchange_weak(low, start)) {
                            break;
                        }
                    }
                    for (std::uintptr_t high = highest.load(); start + block_size > high;) {
                        if (highest.compare_exchange_weak(high, start + block_size)) {
                            break;
                        }
                    }
                    block.uses.store(0);
                    {
                        const std::scoped_lock lock{block.mutex};
                        block.address = start;
                    }
                    // Writes all over the block until the GPU thread used it a few times: those
                    // to a page of a tracked image fault.
                    auto* const words = static_cast<u64*>(d.address);
                    const u32 enough = 1 + random() % 4;
                    for (u64 i = 0; block.uses.load(std::memory_order_relaxed) < enough &&
                                    !stop.load(std::memory_order_relaxed);
                         ++i) {
                        std::atomic_ref<u64>(words[random() % block_words])
                            .store(i, std::memory_order_relaxed);
                    }
                    {
                        const std::scoped_lock lock{block.mutex};
                        block.address = 0;
                    }
                    d.unmap();
                    unmaps.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        // The images over the blocks. Only the GPU thread may walk the cache without its lock.
        const auto images = [&] {
            u64 count = 0;
            const std::uintptr_t low = lowest.load();
            if (low < highest.load()) {
                textures.ForEachImageInRegion(
                    low, highest.load() - low,
                    [&](VideoCore::ImageId, VideoCore::Image&) { ++count; });
            }
            return count;
        };
        // One step of the GPU thread, on Liverpool's thread as in a game run: it finds an image
        // in a mapped block, tracks and uploads it and runs the collector (which frees nothing
        // here: the device is far from the memory use that starts it). Then it looks at the
        // cache twice, a moment apart: an image that left in between was freed by another
        // thread, in the middle of what the GPU thread was doing.
        std::mt19937_64 random(99);
        u64 steps = 0;
        u64 changed_under_gpu_thread = 0;
        const auto step = [&] {
            Block& block = blocks[random() % guests];
            {
                const std::scoped_lock lock{block.mutex};
                if (block.address != 0) {
                    VideoCore::TextureCache::ImageDesc desc{
                        group, block.address + (random() % block_images) * image_size};
                    textures.UpdateImage(textures.FindImage(desc));
                    block.uses.fetch_add(1, std::memory_order_relaxed);
                }
            }
            textures.RunGarbageCollector();
            const u64 before = images();
            const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(20);
            while (std::chrono::steady_clock::now() < until) {
            }
            changed_under_gpu_thread += images() != before ? 1 : 0;
            if (++steps % 64 == 0) {
                gpu.rasterizer.Finish();  // runs the deferred deletions
            }
        };
        constexpr u64 min_steps = 5000;
        constexpr u64 min_unmaps = 1000;
        constexpr u64 min_faults = 1000;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (steps < min_steps || unmaps.load() < min_unmaps ||
               bb::shadps4::claimed_faults() - claimed < min_faults) {
            if (std::chrono::steady_clock::now() > deadline) {
                break;
            }
            gpu.liverpool->SendCommand<true>(step);
        }
        stop.store(true);
        for (auto& thread : threads) {
            thread.join();
        }
        EXPECT_EQ(changed_under_gpu_thread, 0u) << steps << " steps";
        EXPECT_GE(steps, min_steps);
        EXPECT_GE(unmaps.load(), min_unmaps) << steps << " steps";
        EXPECT_GE(bb::shadps4::claimed_faults() - claimed, min_faults) << steps << " steps";

        u64 left = 0;
        gpu.liverpool->SendCommand<true>([&] {
            left = images();
            gpu.rasterizer.Finish();
        });
        EXPECT_EQ(left, 0u);
    }
}

// The contents of one layer of a linear 64 x 64 image as the GPU holds them, read back through a
// staging buffer. On the GPU thread.
std::vector<u8> read_back(Gpu& gpu, VideoCore::Image& image, u32 layer = 0,
                          vk::ImageAspectFlagBits aspect = vk::ImageAspectFlagBits::eColor) {
    const u32 size = image.info.guest_size;
    auto& pool = gpu.runtime.GetStagingPool();
    const auto staging = pool.Request(size, VideoCore::MemoryType::HostCached, 16, true);
    const vk::BufferImageCopy copy{
        .bufferOffset = staging.offset,
        .imageSubresource{.aspectMask = aspect,
                          .baseArrayLayer = layer,
                          .layerCount = 1},
        .imageExtent = {image.info.size.width, image.info.size.height, 1},
    };
    gpu.runtime.DownloadImage(&image, staging.buffer, std::span{&copy, 1});
    gpu.rasterizer.Finish();
    staging.Invalidate();
    std::vector<u8> bytes(staging.mapped, staging.mapped + size);
    pool.FreeDeferred(staging);
    return bytes;
}

// A guest write to a tracked image's pages faults on its guest thread and marks the image written
// by the CPU, while the GPU thread uploads the image (#631). Upstream clears the dirty bits after
// the copy from guest memory, with plain read-modify-writes, so a write whose fault came around
// the copy was lost: the image stayed stale and untracked until the next write to its memory.
// Guest threads write the images while the GPU thread finds, uploads and collects them, with the
// collector set to free whatever went unused for a while (on lavapipe its memory thresholds are
// never met), so it frees images under the faults too. Each time the writers stop, one more
// UpdateImage has to make every image equal to its guest memory.
TEST(GuestMemory, GuestWritesDuringUploadsAreNotLost) {
    if (const std::string reason = no_vulkan_reason(); !reason.empty()) {
        GTEST_SKIP() << reason;
    }
    {
        Gpu gpu;
        auto& textures = gpu.rasterizer.GetTextureCache();
        textures.SetGcThresholds(0, 0, 0);
        Libraries::VideoOut::BufferAttributeGroup group{};
        group.is_occupied = true;
        group.attrib.pixel_format = Libraries::VideoOut::PixelFormat::A8R8G8B8Srgb;
        group.attrib.tiling_mode = Libraries::VideoOut::TilingMode::Linear;
        group.attrib.width = group.attrib.height = group.attrib.pitch_in_pixel = 64;
        constexpr u64 image_size = 64 * 64 * 4;
        constexpr u64 images = 64;  // so that some go unused long enough for the collector
        constexpr u64 image_words = image_size / sizeof(u64);
        Direct d(images * image_size / page_size);
        const std::uintptr_t start = addr(d.address);
        auto* const words = static_cast<u64*>(d.address);
        const auto find = [&](u64 i) {
            VideoCore::TextureCache::ImageDesc desc{group, start + i * image_size};
            return textures.FindImage(desc);
        };

        const std::uint64_t claimed = bb::shadps4::claimed_faults();
        std::mt19937_64 random(99);
        std::array<u64, images> uids{};
        u64 steps = 0;
        u64 freed = 0;
        u64 stale = 0;
        constexpr int rounds = 20;
        constexpr u64 min_steps = 400;
        constexpr u64 min_faults = 400;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
        for (int round = 0; round < rounds; ++round) {
            std::atomic<bool> stop{false};
            std::vector<std::thread> threads;
            for (int t = 0; t < 4; ++t) {
                threads.emplace_back([&, t] {
                    std::mt19937_64 writer(round * 16 + t + 1);
                    for (u64 n = 0; !stop.load(std::memory_order_relaxed); ++n) {
                        const u64 at = (writer() % images) * image_words + writer() % image_words;
                        std::atomic_ref<u64>(words[at]).store((u64(t) << 56) | n,
                                                              std::memory_order_relaxed);
                        // A short pause of random length, so the faults land anywhere in an
                        // upload, not only right after the GPU thread tracked the image.
                        for (u64 spin = writer() % 256; spin > 0; --spin) {
                            asm volatile("" ::: "memory");
                        }
                    }
                });
            }
            const u64 round_faults = bb::shadps4::claimed_faults();
            for (u64 step = 0; step < min_steps ||
                               bb::shadps4::claimed_faults() - round_faults < min_faults;
                 ++step) {
                if (std::chrono::steady_clock::now() > deadline) {
                    break;
                }
                gpu.liverpool->SendCommand<true>([&] {
                    textures.UpdateImage(find(random() % images));
                    textures.RunGarbageCollector();
                    if (++steps % 32 == 0) {
                        gpu.rasterizer.Finish();  // runs the deferred deletions
                    }
                });
            }
            stop.store(true);
            for (auto& thread : threads) {
                thread.join();
            }
            gpu.liverpool->SendCommand<true>([&] {
                for (u64 i = 0; i < images; ++i) {
                    const VideoCore::ImageId id = find(i);
                    textures.UpdateImage(id);
                    VideoCore::Image& image = textures.GetImage(id);
                    freed += uids[i] != 0 && uids[i] != image.image_uid ? 1 : 0;
                    uids[i] = image.image_uid;
                    const std::vector<u8> bytes = read_back(gpu, image);
                    stale += std::memcmp(bytes.data(), &words[i * image_words], image_size) != 0
                                 ? 1
                                 : 0;
                }
            });
        }
        EXPECT_EQ(stale, 0u) << steps << " steps";
        EXPECT_GE(steps, rounds * min_steps);
        EXPECT_GE(bb::shadps4::claimed_faults() - claimed, rounds * min_faults);
        EXPECT_GT(freed, 0u) << steps << " steps";
        gpu.liverpool->SendCommand<true>([&] { gpu.rasterizer.Finish(); });
        d.unmap();
    }
}

// The same for a merge (#689): a texture whose second layer the cache holds as an image of its own
// is made as a new image, uploaded from guest memory, and the old image is copied into its second
// layer. Upstream uploads the new image before it tracks it and clears its dirty bits after the
// copy, so a guest write to the first layer in between was lost and the texture stayed stale. Guest
// threads write the first layer while the GPU thread merges, and once they stop, one UpdateImage
// has to make that layer equal to its guest memory.
TEST(GuestMemory, GuestWritesDuringAMergeAreNotLost) {
    if (const std::string reason = no_vulkan_reason(); !reason.empty()) {
        GTEST_SKIP() << reason;
    }
    {
        Gpu gpu;
        auto& textures = gpu.rasterizer.GetTextureCache();
        Libraries::VideoOut::BufferAttributeGroup group{};
        group.is_occupied = true;
        group.attrib.pixel_format = Libraries::VideoOut::PixelFormat::A8R8G8B8Srgb;
        group.attrib.tiling_mode = Libraries::VideoOut::TilingMode::Linear;
        group.attrib.width = group.attrib.height = group.attrib.pitch_in_pixel = 64;
        constexpr u64 image_size = 64 * 64 * 4;
        constexpr u64 image_words = image_size / sizeof(u64);
        constexpr int rounds = 40;
        // Each round merges at a fresh place: the texture's two layers, rounded up to pages.
        const u64 stride = (2 * image_size + page_size - 1) / page_size * page_size;
        Direct d(rounds * stride / page_size);
        const std::uintptr_t start = addr(d.address);

        const std::uint64_t claimed = bb::shadps4::claimed_faults();
        u64 merged = 0;
        u64 stale = 0;
        for (int round = 0; round < rounds; ++round) {
            const std::uintptr_t texture = start + round * stride;
            auto* const words = reinterpret_cast<u64*>(texture);
            VideoCore::TextureCache::ImageDesc layer1{group, texture + image_size};
            gpu.liverpool->SendCommand<true>(
                [&] { textures.UpdateImage(textures.FindImage(layer1)); });

            std::atomic<bool> stop{false};
            std::atomic<int> started{0};
            std::vector<std::thread> threads;
            for (int t = 0; t < 4; ++t) {
                threads.emplace_back([&, t] {
                    std::mt19937_64 writer(round * 16 + t + 1);
                    started.fetch_add(1);
                    for (u64 n = 0; !stop.load(std::memory_order_relaxed); ++n) {
                        std::atomic_ref<u64>(words[writer() % image_words])
                            .store((u64(t) << 56) | n, std::memory_order_relaxed);
                        for (u64 spin = writer() % 256; spin > 0; --spin) {
                            asm volatile("" ::: "memory");
                        }
                    }
                });
            }
            while (started.load() < 4) {
                std::this_thread::yield();
            }
            VideoCore::ImageId id{};
            const std::uint64_t before_merge = bb::shadps4::claimed_faults();
            gpu.liverpool->SendCommand<true>([&] {
                VideoCore::TextureCache::ImageDesc both{group, texture};
                both.info.resources.layers = 2;
                both.info.UpdateSize();
                id = textures.FindImage(both);
                merged += textures.GetImage(id).info.resources.layers == 2 ? 1 : 0;
            });
            // The writers keep going until one of their writes hit the tracked first layer, so
            // every round races a write against the merge, also on a slow machine where 2 ms
            // weren't always enough (the Ubuntu CI job, 39 of 40 rounds).
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (bb::shadps4::claimed_faults() == before_merge &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            stop.store(true);
            for (auto& thread : threads) {
                thread.join();
            }
            gpu.liverpool->SendCommand<true>([&] {
                textures.UpdateImage(id);
                const std::vector<u8> bytes = read_back(gpu, textures.GetImage(id));
                stale += std::memcmp(bytes.data(), words, image_size) != 0 ? 1 : 0;
            });
        }
        EXPECT_EQ(stale, 0u);
        EXPECT_EQ(merged, u64(rounds));
        EXPECT_GE(bb::shadps4::claimed_faults() - claimed, u64(rounds));
        gpu.liverpool->SendCommand<true>([&] { gpu.rasterizer.Finish(); });
        d.unmap();
    }
}

// A GPU clear keeps the image it wrote when a guest write to a neighbour on the image's first page
// came between the tracking and the clear (#689). That write untracks only the shared page and
// marks nothing, so the clear's dirty bits go, and the next UpdateImage leaves the image alone. A
// write to the image's own bytes after that is found by TrackImage's hash of the page.
TEST(GuestMemory, ANeighbourWriteKeepsAGpuClear) {
    if (const std::string reason = no_vulkan_reason(); !reason.empty()) {
        GTEST_SKIP() << reason;
    }
    {
        Gpu gpu;
        auto& textures = gpu.rasterizer.GetTextureCache();
        Libraries::VideoOut::BufferAttributeGroup group{};
        group.is_occupied = true;
        group.attrib.pixel_format = Libraries::VideoOut::PixelFormat::A8R8G8B8Srgb;
        group.attrib.tiling_mode = Libraries::VideoOut::TilingMode::Linear;
        group.attrib.width = group.attrib.height = group.attrib.pitch_in_pixel = 64;
        constexpr u64 image_size = 64 * 64 * 4;
        constexpr u64 neighbour = 256;  // bytes before the image on its first page
        Direct d((neighbour + image_size + page_size - 1) / page_size + 1);
        const std::uintptr_t start = addr(d.address);
        auto* const bytes = static_cast<u8*>(d.address);
        std::memset(bytes + neighbour, 0x11, image_size);
        const vk::ClearValue clear{.color = {.uint32 = std::array<u32, 4>{0, 0, 0, 0}}};

        gpu.liverpool->SendCommand<true>([&] {
            VideoCore::TextureCache::ImageDesc desc{group, start + neighbour};
            const VideoCore::ImageId id = textures.FindImage(desc);
            VideoCore::Image& image = textures.GetImage(id);
            textures.TrackImage(id);
            ASSERT_TRUE(True(image.flags & VideoCore::ImageFlagBits::Dirty));
            std::atomic_ref<u8>(bytes[0]).store(0x22, std::memory_order_relaxed);
            {
                std::scoped_lock lk{image.mutex};
                EXPECT_FALSE(image.IsUntracked());
                EXPECT_FALSE(image.IsFullyTracked());  // the neighbour's page is untracked
            }
            gpu.runtime.ClearImage(&image, {.base = {}, .extent = image.info.resources}, clear);
            EXPECT_TRUE(False(image.flags & VideoCore::ImageFlagBits::Dirty));
            textures.UpdateImage(id);
            const std::vector<u8> cleared = read_back(gpu, image);
            EXPECT_TRUE(std::all_of(cleared.begin(), cleared.begin() + image_size,
                                    [](u8 b) { return b == 0; }));

            // Again, and after the clear the guest writes the image's own bytes on the untracked
            // page: no fault, the edge hash finds them.
            std::atomic_ref<u8>(bytes[0]).store(0x44, std::memory_order_relaxed);
            gpu.runtime.ClearImage(&image, {.base = {}, .extent = image.info.resources}, clear);
            std::atomic_ref<u8>(bytes[neighbour]).store(0x33, std::memory_order_relaxed);
            textures.UpdateImage(id);
            const std::vector<u8> written = read_back(gpu, image);
            EXPECT_EQ(std::memcmp(written.data(), bytes + neighbour, image_size), 0);
            gpu.rasterizer.Finish();
        });
        d.unmap();
    }
}

// A resolve into layer 1 of a two-layer render target clears the target's dirty bits (#689).
// A guest write to layer 0 that the cache has not uploaded yet must still reach the image (#691):
// Rasterizer::Resolve brings the target up to date before it. Upstream only tracks it, and the
// write is lost. Driven through Draw with the resolve registers, as the game does.
TEST(GuestMemory, AResolveIntoOneLayerKeepsAGuestWriteToAnother) {
    if (const std::string reason = no_vulkan_reason(); !reason.empty()) {
        GTEST_SKIP() << reason;
    }
    {
        Gpu gpu;
        auto& textures = gpu.rasterizer.GetTextureCache();
        constexpr u64 layer_size = 64 * 64 * 4;
        Direct d((3 * layer_size + page_size - 1) / page_size);
        const std::uintptr_t start = addr(d.address);
        ASSERT_EQ(start % 256, 0u);
        ASSERT_LT(start >> 8, u64(1) << 32);
        auto* const bytes = static_cast<u8*>(d.address);
        std::memset(bytes, 0x11, 2 * layer_size);  // the target's two layers
        std::memset(bytes + 2 * layer_size, 0x55, layer_size);  // the resolve's source

        // A linear 64 x 64 RGBA8 render target at `at`, its view on layer `layer` of `layer + 1`.
        const auto color_buffer = [](std::uintptr_t at, u32 layer) {
            AmdGpu::ColorBuffer buffer{};
            buffer.base_address = u32(at >> 8);
            buffer.pitch.tile_max = 64 / 8 - 1;
            buffer.slice.tile_max = 64 * 64 / 64 - 1;
            buffer.view.slice_start = layer;
            buffer.view.slice_max = layer;
            buffer.info.format = u32(AmdGpu::DataFormat::Format8_8_8_8);
            buffer.info.number_type = u32(AmdGpu::NumberFormat::Unorm);
            buffer.attrib.tile_mode_index = AmdGpu::TileMode::DisplayLinearAligned;
            return buffer;
        };
        const AmdGpu::ColorBuffer source = color_buffer(start + 2 * layer_size, 0);
        const AmdGpu::ColorBuffer target = color_buffer(start, 1);

        VideoCore::ImageId id{};
        gpu.liverpool->SendCommand<true>([&] {
            VideoCore::TextureCache::ImageDesc source_desc{source, {}};
            textures.UpdateImage(textures.FindImage(source_desc));
            VideoCore::TextureCache::ImageDesc target_desc{target, {}};
            id = textures.FindImage(target_desc);
            ASSERT_EQ(textures.GetImage(id).info.resources.layers, 2u);
            textures.UpdateImage(id);
            ASSERT_TRUE(False(textures.GetImage(id).flags & VideoCore::ImageFlagBits::Dirty));
            gpu.rasterizer.Finish();
        });

        // The guest writes layer 0 of the tracked target: it faults and marks it.
        for (u64 i = 0; i < layer_size; i += 64) {
            std::atomic_ref<u8>(bytes[i]).store(0x77, std::memory_order_relaxed);
        }
        gpu.liverpool->SendCommand<true>([&] {
            VideoCore::Image& image = textures.GetImage(id);
            EXPECT_TRUE(True(image.flags & VideoCore::ImageFlagBits::CpuDirty));
            auto& regs = gpu.liverpool->regs;
            regs.color_control.mode = AmdGpu::ColorControl::OperationMode::Resolve;
            regs.color_buffers[0] = source;
            regs.color_buffers[1] = target;
            gpu.rasterizer.Draw(false);
            regs.color_control.mode = AmdGpu::ColorControl::OperationMode::Normal;
            regs.color_buffers[0] = {};
            regs.color_buffers[1] = {};

            textures.UpdateImage(id);
            const std::vector<u8> layer0 = read_back(gpu, image, 0);
            EXPECT_EQ(std::memcmp(layer0.data(), bytes, layer_size), 0);
            const std::vector<u8> layer1 = read_back(gpu, image, 1);
            EXPECT_TRUE(std::all_of(layer1.begin(), layer1.begin() + layer_size,
                                    [](u8 b) { return b == 0x55; }));
            gpu.rasterizer.Finish();
        });
        d.unmap();
    }
}

// A linear 64 x 64 RGBA8 render target at `at`, its view on layer `layer` of `layer + 1`.
AmdGpu::ColorBuffer linear_color_buffer(std::uintptr_t at, u32 layer, u32 size = 64) {
    AmdGpu::ColorBuffer buffer{};
    buffer.base_address = u32(at >> 8);
    buffer.pitch.tile_max = size / 8 - 1;
    buffer.slice.tile_max = size * size / 64 - 1;
    buffer.view.slice_start = layer;
    buffer.view.slice_max = layer;
    buffer.info.format = u32(AmdGpu::DataFormat::Format8_8_8_8);
    buffer.info.number_type = u32(AmdGpu::NumberFormat::Unorm);
    buffer.attrib.tile_mode_index = AmdGpu::TileMode::DisplayLinearAligned;
    return buffer;
}

// A 64 x 64 Z32 depth buffer (1D tiled; every depth buffer is tiled) that reads at `read`,
// writes at `write`.
AmdGpu::DepthBuffer depth_buffer(std::uintptr_t read, std::uintptr_t write) {
    AmdGpu::DepthBuffer buffer{};
    buffer.z_info.format = AmdGpu::DepthBuffer::ZFormat::Z32Float;
    buffer.z_info.tile_mode_index = u32(AmdGpu::TileMode::Depth1DThin);
    buffer.z_read_base = u32(read >> 8);
    buffer.z_write_base = u32(write >> 8);
    buffer.depth_size.pitch_tile_max = 64 / 8 - 1;
    buffer.depth_size.height_tile_max = 64 / 8 - 1;
    buffer.depth_slice.tile_max = 64 * 64 / 64 - 1;
    return buffer;
}

// Fills `bytes` with the float `value`, so that the data reads the same however it is tiled.
void fill_floats(u8* bytes, u64 size, float value) {
    for (u64 i = 0; i < size; i += sizeof(float)) {
        std::memcpy(bytes + i, &value, sizeof(float));
    }
}

testing::AssertionResult all_floats(const std::vector<u8>& bytes, u64 size, float value) {
    for (u64 i = 0; i < size; i += sizeof(float)) {
        if (std::memcmp(bytes.data() + i, &value, sizeof(float)) != 0) {
            float got;
            std::memcpy(&got, bytes.data() + i, sizeof(float));
            return testing::AssertionFailure() << "byte " << i << " holds " << got;
        }
    }
    return testing::AssertionSuccess();
}

// The source of a resolve (here a single-sampled one, a plain copy) is brought up to date before
// it (#696). Upstream copies the stale source, and since the copy clears the target's dirty bits,
// the guest's write to the source never reaches the target.
TEST(GuestMemory, AResolveTakesAGuestWriteToItsSource) {
    if (const std::string reason = no_vulkan_reason(); !reason.empty()) {
        GTEST_SKIP() << reason;
    }
    {
        Gpu gpu;
        auto& textures = gpu.rasterizer.GetTextureCache();
        constexpr u64 image_size = 64 * 64 * 4;
        Direct d((2 * image_size + page_size - 1) / page_size);
        const std::uintptr_t start = addr(d.address);
        ASSERT_EQ(start % 256, 0u);
        ASSERT_LT(start >> 8, u64(1) << 32);
        auto* const bytes = static_cast<u8*>(d.address);
        std::memset(bytes, 0x11, image_size);               // the target
        std::memset(bytes + image_size, 0x55, image_size);  // the source
        const AmdGpu::ColorBuffer source = linear_color_buffer(start + image_size, 0);
        const AmdGpu::ColorBuffer target = linear_color_buffer(start, 0);

        VideoCore::ImageId id{};
        gpu.liverpool->SendCommand<true>([&] {
            VideoCore::TextureCache::ImageDesc source_desc{source, {}};
            const auto source_id = textures.FindImage(source_desc);
            textures.UpdateImage(source_id);
            ASSERT_TRUE(
                False(textures.GetImage(source_id).flags & VideoCore::ImageFlagBits::Dirty));
            VideoCore::TextureCache::ImageDesc target_desc{target, {}};
            id = textures.FindImage(target_desc);
            textures.UpdateImage(id);
            gpu.rasterizer.Finish();
        });

        // The guest writes the tracked source: it faults and marks it.
        for (u64 i = 0; i < image_size; ++i) {
            std::atomic_ref<u8>(bytes[image_size + i]).store(0x77, std::memory_order_relaxed);
        }
        gpu.liverpool->SendCommand<true>([&] {
            auto& regs = gpu.liverpool->regs;
            regs.color_control.mode = AmdGpu::ColorControl::OperationMode::Resolve;
            regs.color_buffers[0] = source;
            regs.color_buffers[1] = target;
            gpu.rasterizer.Draw(false);
            regs.color_control.mode = AmdGpu::ColorControl::OperationMode::Normal;
            regs.color_buffers[0] = {};
            regs.color_buffers[1] = {};

            VideoCore::Image& image = textures.GetImage(id);
            textures.UpdateImage(id);
            const std::vector<u8> copied = read_back(gpu, image);
            EXPECT_TRUE(std::all_of(copied.begin(), copied.begin() + image_size,
                                    [](u8 b) { return b == 0x77; }));
            gpu.rasterizer.Finish();
        });
        d.unmap();
    }
}

// The same for the depth copy a draw with color disabled and forced depth dirty and valid does
// (Rasterizer::DepthStencilCopy, #696).
TEST(GuestMemory, ADepthCopyTakesAGuestWriteToItsSource) {
    if (const std::string reason = no_vulkan_reason(); !reason.empty()) {
        GTEST_SKIP() << reason;
    }
    {
        Gpu gpu;
        auto& textures = gpu.rasterizer.GetTextureCache();
        constexpr u64 image_size = 64 * 64 * 4;
        Direct d((2 * image_size + page_size - 1) / page_size);
        const std::uintptr_t start = addr(d.address);
        ASSERT_EQ(start % 256, 0u);
        ASSERT_LT(start >> 8, u64(1) << 32);
        auto* const bytes = static_cast<u8*>(d.address);
        fill_floats(bytes, image_size, 0.25f);               // the destination
        fill_floats(bytes + image_size, image_size, 0.5f);   // the source
        const AmdGpu::DepthBuffer buffer = depth_buffer(start + image_size, start);
        const AmdGpu::DepthView view{};
        const AmdGpu::DepthControl control{};

        VideoCore::ImageId id{};
        gpu.liverpool->SendCommand<true>([&] {
            VideoCore::TextureCache::ImageDesc read_desc{buffer, view, control, 0, {}, false};
            const auto read_id = textures.FindImage(read_desc);
            textures.UpdateImage(read_id);
            ASSERT_TRUE(False(textures.GetImage(read_id).flags & VideoCore::ImageFlagBits::Dirty));
            VideoCore::TextureCache::ImageDesc write_desc{buffer, view, control, 0, {}, true};
            id = textures.FindImage(write_desc);
            ASSERT_NE(id, read_id);
            textures.UpdateImage(id);
            gpu.rasterizer.Finish();
        });

        // The guest writes the tracked source: it faults and marks it.
        for (u64 i = 0; i < image_size; i += sizeof(float)) {
            constexpr float value = 0.75f;
            u32 word;
            std::memcpy(&word, &value, sizeof(word));
            std::atomic_ref<u32>(*reinterpret_cast<u32*>(bytes + image_size + i))
                .store(word, std::memory_order_relaxed);
        }
        gpu.liverpool->SendCommand<true>([&] {
            auto& regs = gpu.liverpool->regs;
            regs.color_control.mode = AmdGpu::ColorControl::OperationMode::Disable;
            regs.primitive_type = AmdGpu::PrimitiveType::TriangleList;
            regs.depth_render_override.force_z_dirty = 1;
            regs.depth_render_override.force_z_valid = 1;
            regs.depth_buffer = buffer;
            regs.depth_view = view;
            gpu.rasterizer.Draw(false);
            regs.color_control.mode = AmdGpu::ColorControl::OperationMode::Normal;
            regs.primitive_type = AmdGpu::PrimitiveType::None;
            regs.depth_render_override = {};
            regs.depth_buffer = {};

            VideoCore::Image& image = textures.GetImage(id);
            textures.UpdateImage(id);
            const std::vector<u8> copied =
                read_back(gpu, image, 0, vk::ImageAspectFlagBits::eDepth);
            EXPECT_TRUE(all_floats(copied, image_size, 0.75f));
            gpu.rasterizer.Finish();
        });
        d.unmap();
    }
}

// A depth target found where the cache holds a color image of the same pitch and height but a
// narrower width (from the draw's extent hint) is recreated as a depth image, and the color image
// is copied into it (TextureCache::ResolveDepthOverlap). The copy covers only the color image's
// width, so the rest is uploaded from guest memory first (#696). Upstream uploads it only for more
// layers or levels, and left the rest undefined but marked clean. (Images of another pitch or
// height at the same address take the pool reuse path in ResolveOverlap and never get here.)
TEST(GuestMemory, ADepthImageRecreatedWiderHoldsTheGuestData) {
    if (const std::string reason = no_vulkan_reason(); !reason.empty()) {
        GTEST_SKIP() << reason;
    }
    {
        Gpu gpu;
        auto& textures = gpu.rasterizer.GetTextureCache();
        constexpr u64 image_size = 64 * 64 * 4;
        Direct d((image_size + page_size - 1) / page_size);
        const std::uintptr_t start = addr(d.address);
        ASSERT_EQ(start % 256, 0u);
        ASSERT_LT(start >> 8, u64(1) << 32);
        auto* const bytes = static_cast<u8*>(d.address);
        fill_floats(bytes, image_size, 0.375f);
        AmdGpu::ColorBuffer color = linear_color_buffer(start, 0);
        color.info.format = u32(AmdGpu::DataFormat::Format32);
        color.info.number_type = u32(AmdGpu::NumberFormat::Float);
        AmdGpu::CbDbExtent narrow{};
        narrow.width = 32;
        narrow.height = 64;
        const AmdGpu::DepthBuffer depth = depth_buffer(start, start);

        gpu.liverpool->SendCommand<true>([&] {
            VideoCore::TextureCache::ImageDesc color_desc{color, narrow};
            const auto color_id = textures.FindImage(color_desc);
            textures.UpdateImage(color_id);
            ASSERT_EQ(textures.GetImage(color_id).info.size.width, 32u);
            ASSERT_EQ(textures.GetImage(color_id).info.guest_size, image_size);

            VideoCore::TextureCache::ImageDesc depth_desc{depth, {}, {}, 0, {}, false};
            const auto id = textures.FindImage(depth_desc);
            ASSERT_NE(id, color_id);
            VideoCore::Image& image = textures.GetImage(id);
            ASSERT_TRUE(image.info.props.is_depth);
            ASSERT_EQ(image.info.size.width, 64u);
            textures.UpdateImage(id);
            const std::vector<u8> held = read_back(gpu, image, 0, vk::ImageAspectFlagBits::eDepth);
            EXPECT_TRUE(all_floats(held, image_size, 0.375f));
            gpu.rasterizer.Finish();
        });
        d.unmap();
    }
}

// The color image a recreated depth image is copied from is brought up to date first (#696):
// here it is as wide as the depth image, so the copy covers all of it and nothing else uploads it.
TEST(GuestMemory, ADepthImageRecreatedTakesAGuestWriteToTheOldImage) {
    if (const std::string reason = no_vulkan_reason(); !reason.empty()) {
        GTEST_SKIP() << reason;
    }
    {
        Gpu gpu;
        auto& textures = gpu.rasterizer.GetTextureCache();
        constexpr u64 image_size = 64 * 64 * 4;
        Direct d((image_size + page_size - 1) / page_size);
        const std::uintptr_t start = addr(d.address);
        ASSERT_EQ(start % 256, 0u);
        ASSERT_LT(start >> 8, u64(1) << 32);
        auto* const bytes = static_cast<u8*>(d.address);
        fill_floats(bytes, image_size, 0.375f);
        AmdGpu::ColorBuffer color = linear_color_buffer(start, 0);
        color.info.format = u32(AmdGpu::DataFormat::Format32);
        color.info.number_type = u32(AmdGpu::NumberFormat::Float);
        const AmdGpu::DepthBuffer depth = depth_buffer(start, start);

        VideoCore::ImageId color_id{};
        gpu.liverpool->SendCommand<true>([&] {
            VideoCore::TextureCache::ImageDesc color_desc{color, {}};
            color_id = textures.FindImage(color_desc);
            textures.UpdateImage(color_id);
            ASSERT_TRUE(False(textures.GetImage(color_id).flags & VideoCore::ImageFlagBits::Dirty));
            gpu.rasterizer.Finish();
        });

        // The guest writes the tracked color image: it faults and marks it.
        for (u64 i = 0; i < image_size; i += sizeof(float)) {
            constexpr float value = 0.625f;
            u32 word;
            std::memcpy(&word, &value, sizeof(word));
            std::atomic_ref<u32>(*reinterpret_cast<u32*>(bytes + i))
                .store(word, std::memory_order_relaxed);
        }
        gpu.liverpool->SendCommand<true>([&] {
            VideoCore::TextureCache::ImageDesc depth_desc{depth, {}, {}, 0, {}, false};
            const auto id = textures.FindImage(depth_desc);
            ASSERT_NE(id, color_id);
            VideoCore::Image& image = textures.GetImage(id);
            ASSERT_TRUE(image.info.props.is_depth);
            textures.UpdateImage(id);
            const std::vector<u8> held = read_back(gpu, image, 0, vk::ImageAspectFlagBits::eDepth);
            EXPECT_TRUE(all_floats(held, image_size, 0.625f));
            gpu.rasterizer.Finish();
        });
        d.unmap();
    }
}

// Whether the Khronos validation layer can be loaded (CI has none; locally VK_ADD_LAYER_PATH).
// Asks the loader directly: vulkan.hpp's dispatcher is set up only by the first instance.
bool validation_layer_present() {
    void* loader = ::dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (loader == nullptr) {
        return false;
    }
    const auto get_proc =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(::dlsym(loader, "vkGetInstanceProcAddr"));
    const auto enumerate = reinterpret_cast<PFN_vkEnumerateInstanceLayerProperties>(
        get_proc != nullptr ? get_proc(nullptr, "vkEnumerateInstanceLayerProperties") : nullptr);
    std::vector<VkLayerProperties> layers;
    uint32_t count = 0;
    if (enumerate != nullptr && enumerate(&count, nullptr) == VK_SUCCESS) {
        layers.resize(count);
        if (enumerate(&count, layers.data()) != VK_SUCCESS) {
            layers.clear();
        }
        layers.resize(std::min<std::size_t>(count, layers.size()));
    }
    ::dlclose(loader);
    return std::ranges::any_of(layers, [](const VkLayerProperties& layer) {
        return std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0;
    });
}

// Counts the validation layer's errors on `instance` while it lives, and prints them.
struct ValidationErrors {
    std::atomic<u32> count{0};
    vk::UniqueDebugUtilsMessengerEXT messenger;

    explicit ValidationErrors(vk::Instance instance) {
        const vk::DebugUtilsMessengerCreateInfoEXT info{
            .messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eError,
            .messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation,
            .pfnUserCallback = &ValidationErrors::OnMessage,
            .pUserData = this,
        };
        auto [result, created] = instance.createDebugUtilsMessengerEXTUnique(info);
        EXPECT_EQ(result, vk::Result::eSuccess);
        messenger = std::move(created);
    }

    static VKAPI_ATTR vk::Bool32 VKAPI_CALL
    OnMessage(vk::DebugUtilsMessageSeverityFlagBitsEXT, vk::DebugUtilsMessageTypeFlagsEXT,
              const vk::DebugUtilsMessengerCallbackDataEXT* data, void* user) {
        static_cast<ValidationErrors*>(user)->count.fetch_add(1, std::memory_order_relaxed);
        std::fprintf(stderr, "validation: %s\n",
                     data != nullptr && data->pMessage != nullptr ? data->pMessage : "?");
        return vk::False;
    }
};

// A depth target of 6 layers requested at the address of a texture of 2 levels and 1 layer is
// recreated as a depth image (TextureCache::ResolveDepthOverlap) with the larger count of each:
// 2 levels and 6 layers (#699). Upstream took the larger extent as a whole, levels first, and
// recreated it with 2 levels and 1 layer, so the 6 layers the guest renders to were missing.
TEST(GuestMemory, ADepthImageRecreatedFromFewerLayersHasTheRequestedLayers) {
    if (const std::string reason = no_vulkan_reason(); !reason.empty()) {
        GTEST_SKIP() << reason;
    }
    {
        Gpu gpu;
        auto& textures = gpu.rasterizer.GetTextureCache();
        constexpr u32 layers = 6;
        constexpr u64 slice_size = 64 * 64 * 4;
        // Room for the recreated image's 2 levels of 6 layers.
        constexpr u64 mapped = 4 * layers * slice_size;
        Direct d((mapped + page_size - 1) / page_size);
        const std::uintptr_t start = addr(d.address);
        ASSERT_EQ(start % 256, 0u);
        ASSERT_LT(start >> 8, u64(1) << 32);
        fill_floats(static_cast<u8*>(d.address), mapped, 0.375f);

        AmdGpu::Image texture{};
        texture.base_address = start >> 8;
        texture.data_format = u64(AmdGpu::DataFormat::Format32);
        texture.num_format = u64(AmdGpu::NumberFormat::Float);
        texture.width = 64 - 1;
        texture.height = 64 - 1;
        texture.pitch = 64 - 1;
        texture.last_level = 1;
        texture.tiling_index = u64(AmdGpu::TileMode::Depth1DThin);
        texture.type = u64(AmdGpu::ImageType::Color2D);
        texture.dst_sel_x = u64(AmdGpu::CompSwizzle::Red);
        AmdGpu::DepthBuffer depth = depth_buffer(start, start);
        AmdGpu::DepthView view{};
        view.slice_max = layers - 1;

        gpu.liverpool->SendCommand<true>([&] {
            VideoCore::TextureCache::ImageDesc texture_desc{texture, Shader::ImageResource{}};
            const auto texture_id = textures.FindImage(texture_desc);
            textures.UpdateImage(texture_id);
            ASSERT_EQ(textures.GetImage(texture_id).info.resources.levels, 2u);
            ASSERT_EQ(textures.GetImage(texture_id).info.resources.layers, 1u);

            VideoCore::TextureCache::ImageDesc depth_desc{depth, view, {}, 0, {}, false};
            const auto id = textures.FindImage(depth_desc);
            ASSERT_NE(id, texture_id);
            VideoCore::Image& image = textures.GetImage(id);
            ASSERT_TRUE(image.info.props.is_depth);
            EXPECT_EQ(image.info.resources.levels, 2u);
            ASSERT_EQ(image.info.resources.layers, layers);
            textures.UpdateImage(id);
            const std::vector<u8> held =
                read_back(gpu, image, layers - 1, vk::ImageAspectFlagBits::eDepth);
            EXPECT_TRUE(all_floats(held, slice_size, 0.375f));
            gpu.rasterizer.Finish();
        });
        d.unmap();
    }
}

// A depth target narrower (from the draw's extent hint) than the color image at its address is
// recreated at its own extent, and the copy from the color image is clamped to it (#699). Upstream
// copied the color image's whole extent, out of the depth image's bounds: with the validation
// layer present (VK_ADD_LAYER_PATH) that is an error, which this test counts.
TEST(GuestMemory, ADepthImageRecreatedNarrowerTakesACopyWithinItsBounds) {
    if (const std::string reason = no_vulkan_reason(); !reason.empty()) {
        GTEST_SKIP() << reason;
    }
    const bool validate = validation_layer_present();
    if (!validate) {
        std::fprintf(stderr, "no validation layer: only the data is checked\n");
    }
    {
        Gpu gpu{Vulkan::ValidationOptions{.enable = validate}};
        std::unique_ptr<ValidationErrors> errors;
        if (validate) {
            errors = std::make_unique<ValidationErrors>(gpu.instance.GetInstance());
        }
        auto& textures = gpu.rasterizer.GetTextureCache();
        constexpr u64 image_size = 64 * 64 * 4;
        Direct d((image_size + page_size - 1) / page_size);
        const std::uintptr_t start = addr(d.address);
        ASSERT_EQ(start % 256, 0u);
        ASSERT_LT(start >> 8, u64(1) << 32);
        fill_floats(static_cast<u8*>(d.address), image_size, 0.375f);
        AmdGpu::ColorBuffer color = linear_color_buffer(start, 0);
        color.info.format = u32(AmdGpu::DataFormat::Format32);
        color.info.number_type = u32(AmdGpu::NumberFormat::Float);
        AmdGpu::CbDbExtent narrow{};
        narrow.width = 32;
        narrow.height = 64;
        const AmdGpu::DepthBuffer depth = depth_buffer(start, start);

        gpu.liverpool->SendCommand<true>([&] {
            VideoCore::TextureCache::ImageDesc color_desc{color, {}};
            const auto color_id = textures.FindImage(color_desc);
            textures.UpdateImage(color_id);
            ASSERT_EQ(textures.GetImage(color_id).info.size.width, 64u);

            VideoCore::TextureCache::ImageDesc depth_desc{depth, {}, {}, 0, narrow, false};
            const auto id = textures.FindImage(depth_desc);
            ASSERT_NE(id, color_id);
            VideoCore::Image& image = textures.GetImage(id);
            ASSERT_TRUE(image.info.props.is_depth);
            ASSERT_EQ(image.info.size.width, 32u);
            textures.UpdateImage(id);
            const std::vector<u8> held = read_back(gpu, image, 0, vk::ImageAspectFlagBits::eDepth);
            EXPECT_TRUE(all_floats(held, 32 * 64 * 4, 0.375f));
            gpu.rasterizer.Finish();
        });
        d.unmap();
        if (errors != nullptr) {
            EXPECT_EQ(errors->count.load(), 0u);
        }
    }
}

// The same through CopyImage: a depth target of 2 layers, narrower than the depth image of 1 layer
// at its address, is recreated (more layers) and the old depth image copied into it, clamped to
// the new image's width (#699). The validation layer reports upstream's copy, as above.
TEST(GuestMemory, ADepthImageRecreatedNarrowerFromDepthCopiesWithinItsBounds) {
    if (const std::string reason = no_vulkan_reason(); !reason.empty()) {
        GTEST_SKIP() << reason;
    }
    const bool validate = validation_layer_present();
    {
        Gpu gpu{Vulkan::ValidationOptions{.enable = validate}};
        std::unique_ptr<ValidationErrors> errors;
        if (validate) {
            errors = std::make_unique<ValidationErrors>(gpu.instance.GetInstance());
        }
        auto& textures = gpu.rasterizer.GetTextureCache();
        constexpr u64 slice_size = 64 * 64 * 4;
        Direct d((2 * slice_size + page_size - 1) / page_size);
        const std::uintptr_t start = addr(d.address);
        ASSERT_EQ(start % 256, 0u);
        ASSERT_LT(start >> 8, u64(1) << 32);
        fill_floats(static_cast<u8*>(d.address), 2 * slice_size, 0.375f);
        const AmdGpu::DepthBuffer depth = depth_buffer(start, start);
        AmdGpu::DepthView two_layers{};
        two_layers.slice_max = 1;
        AmdGpu::CbDbExtent narrow{};
        narrow.width = 32;
        narrow.height = 64;

        gpu.liverpool->SendCommand<true>([&] {
            VideoCore::TextureCache::ImageDesc wide_desc{depth, {}, {}, 0, {}, false};
            const auto wide_id = textures.FindImage(wide_desc);
            textures.UpdateImage(wide_id);
            ASSERT_EQ(textures.GetImage(wide_id).info.size.width, 64u);

            VideoCore::TextureCache::ImageDesc narrow_desc{depth, two_layers, {}, 0, narrow, false};
            const auto id = textures.FindImage(narrow_desc);
            ASSERT_NE(id, wide_id);
            VideoCore::Image& image = textures.GetImage(id);
            ASSERT_EQ(image.info.size.width, 32u);
            ASSERT_EQ(image.info.resources.layers, 2u);
            textures.UpdateImage(id);
            for (u32 layer = 0; layer < 2; ++layer) {
                const std::vector<u8> held =
                    read_back(gpu, image, layer, vk::ImageAspectFlagBits::eDepth);
                EXPECT_TRUE(all_floats(held, 32 * 64 * 4, 0.375f)) << "layer " << layer;
            }
            gpu.rasterizer.Finish();
        });
        d.unmap();
        if (errors != nullptr) {
            EXPECT_EQ(errors->count.load(), 0u);
        }
    }
}

// An image found as one layer of a larger image requested later is copied into it
// (TextureCache::ResolveOverlap, left overlap) after the larger image's upload. The layer is
// brought up to date first (#696); upstream copies it stale over the fresh upload.
TEST(GuestMemory, AMergedLayerTakesAGuestWriteToIt) {
    if (const std::string reason = no_vulkan_reason(); !reason.empty()) {
        GTEST_SKIP() << reason;
    }
    {
        Gpu gpu;
        auto& textures = gpu.rasterizer.GetTextureCache();
        constexpr u64 layer_size = 64 * 64 * 4;
        Direct d((2 * layer_size + page_size - 1) / page_size);
        const std::uintptr_t start = addr(d.address);
        ASSERT_EQ(start % 256, 0u);
        ASSERT_LT(start >> 8, u64(1) << 32);
        auto* const bytes = static_cast<u8*>(d.address);
        std::memset(bytes, 0x11, 2 * layer_size);
        const AmdGpu::ColorBuffer layer = linear_color_buffer(start + layer_size, 0);
        const AmdGpu::ColorBuffer both = linear_color_buffer(start, 1);

        VideoCore::ImageId layer_id{};
        gpu.liverpool->SendCommand<true>([&] {
            VideoCore::TextureCache::ImageDesc layer_desc{layer, {}};
            layer_id = textures.FindImage(layer_desc);
            textures.UpdateImage(layer_id);
            ASSERT_TRUE(False(textures.GetImage(layer_id).flags & VideoCore::ImageFlagBits::Dirty));
            gpu.rasterizer.Finish();
        });

        // The guest writes the tracked layer image: it faults and marks it.
        for (u64 i = 0; i < layer_size; ++i) {
            std::atomic_ref<u8>(bytes[layer_size + i]).store(0x77, std::memory_order_relaxed);
        }
        gpu.liverpool->SendCommand<true>([&] {
            VideoCore::TextureCache::ImageDesc both_desc{both, {}};
            const auto id = textures.FindImage(both_desc);
            ASSERT_NE(id, layer_id);
            VideoCore::Image& image = textures.GetImage(id);
            ASSERT_EQ(image.info.resources.layers, 2u);
            textures.UpdateImage(id);
            const std::vector<u8> layer1 = read_back(gpu, image, 1);
            EXPECT_TRUE(std::all_of(layer1.begin(), layer1.begin() + layer_size,
                                    [](u8 b) { return b == 0x77; }));
            gpu.rasterizer.Finish();
        });
        d.unmap();
    }
}

// A rasterizer unregisters from the guest memory when it is destroyed (#629; upstream's never
// does): the maps, unmaps, releases and writes that follow reach no rasterizer.
TEST(GuestMemory, ADestroyedRasterizerHearsOfNothing) {
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
        {
            const auto rasterizer = std::make_unique<Vulkan::Rasterizer>(instance, scheduler,
                                                                         runtime, liverpool.get());
            EXPECT_TRUE(rasterizer->IsMapped(addr(early.address), early.length));
        }
        Direct late(2);
        EXPECT_TRUE(memory().TryWriteBacking(late.address, "backing", 8));
        memory().InvalidateMemory(addr(late.address), 8);
        EXPECT_EQ(sceKernelMprotect(late.address, late.length, 3), 0);
        EXPECT_EQ(release_direct(late.offset + page_size, page_size), 0);
        EXPECT_EQ(sceKernelMunmap(late.address, late.length), 0);
        EXPECT_EQ(release_direct(late.offset, page_size), 0);
        early.unmap();
    }
    liverpool.reset();
}

// A callback that returns true ends the texture cache's page walk (#632): upstream compared the
// invoke_result struct with bool, so a stopping callback still saw every page of the range.
TEST(TextureCachePages, AStoppingCallbackEndsTheWalk) {
    constexpr u64 page = u64{1} << 18;  // TextureCache::Traits::PAGE_BITS (private)
    constexpr u64 start = 0x200000000 + page / 2;
    constexpr size_t size = 7 * page;  // pages 0 to 7 of the range, eight pages
    int stopped = 0;
    VideoCore::TextureCache::ForEachPage(start, size, [&](u64) {
        ++stopped;
        return true;
    });
    EXPECT_EQ(stopped, 1);
    std::vector<u64> seen;
    VideoCore::TextureCache::ForEachPage(start, size, [&](u64 p) {
        seen.push_back(p);
        return seen.size() == 3;
    });
    EXPECT_EQ(seen, (std::vector<u64>{start / page, start / page + 1, start / page + 2}));
    int all = 0;
    VideoCore::TextureCache::ForEachPage(start, size, [&](u64) { ++all; });
    EXPECT_EQ(all, 8);
    int never = 0;
    VideoCore::TextureCache::ForEachPage(start, size, [&](u64) {
        ++never;
        return false;
    });
    EXPECT_EQ(never, 8);
}

}  // namespace
