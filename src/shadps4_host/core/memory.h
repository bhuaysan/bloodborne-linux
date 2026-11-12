// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/core/memory.h, src/core/address_space.h @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-12: only the parts video_core and AvPlayer use, on top of
// our kernel's vmem and dmem (memory.cpp) instead of upstream's address space and memory map
// (docs/design/renderer.md, "One backing for every page the GPU can see").
// License texts: third_party/shadps4/LICENSES/
#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include "common/enum.h"
#include "common/multi_level_page_table.h"
#include "common/types.h"

namespace Vulkan {
class Rasterizer;
}

namespace Core {

// The guest's page size, which the caches hash and track in.
constexpr u64 GUEST_PAGE_BITS = 14;
constexpr u64 GUEST_PAGE_SIZE = 1ULL << GUEST_PAGE_BITS;

enum class MemoryPermission : u32 {
    None = 0,
    Read = 1 << 0,
    Write = 1 << 1,
    ReadWrite = Read | Write,
    Execute = 1 << 2,
    ReadWriteExecute = Read | Write | Execute,
};
DECLARE_ENUM_FLAG_OPERATORS(MemoryPermission)

class AddressSpace {
public:
    // Guest page -> pointer into the backing, null where a page has none.
    struct Traits {
        using Entry = u8*;
        static constexpr size_t ADDRESS_SPACE_BITS = 40;
        static constexpr size_t L1_BITS = 16;
        static constexpr size_t PAGE_BITS = 14;
        static constexpr bool NULL_CHECK = true;
    };
    using BackingPageTable = Common::MultiLevelPageTable<Traits>;

    // The whole direct memory memfd, direct memory then flexible memory, mapped once.
    [[nodiscard]] u8* BackingBase() const noexcept {
        return backing_base;
    }
    // The backing of each guest page of direct and flexible memory below 1 << 40.
    [[nodiscard]] const BackingPageTable& BackingPages() const noexcept {
        return backing_pages;
    }
    // Sets the host protection of guest pages, as the write tracker needs it.
    void Protect(VAddr virtual_addr, u64 size, MemoryPermission perms);

private:
    friend class MemoryManager;
    u8* backing_base{};
    BackingPageTable backing_pages;
};

class MemoryManager {
public:
    // Maps the backing and follows the kernel's mappings from then on, so it is created before
    // the game maps memory.
    MemoryManager();
    ~MemoryManager();

    // The rasterizer registers itself when it is created. Ours: it hears of the GPU-visible
    // mappings that are already there, and null unregisters it. A rasterizer unregisters before it
    // goes; ~Rasterizer doesn't, as upstream's.
    void SetRasterizer(Vulkan::Rasterizer* rasterizer_);

    AddressSpace& GetAddressSpace() {
        return impl;
    }

    bool IsValidGpuMapping(VAddr virtual_addr, u64 size) {
        // The PS4's GPU can only handle 40 bit addresses.
        const VAddr max_gpu_address{0x10000000000};
        return virtual_addr + size < max_gpu_address;
    }

    // Whether the range lies in the guest's address space, mapped or not (memory.cpp).
    bool IsValidMapping(VAddr virtual_addr, u64 size = 0);
    u64 ClampRangeSize(VAddr virtual_addr, u64 size);
    void CopySparseMemory(VAddr source, u8* dest, u64 size);
    bool TryWriteBacking(void* address, const void* data, u64 size);

    // The GPU caches drop what they hold of the range, which host code is about to write
    // (AvPlayer's video frames), as for a system call that writes there (memory.cpp).
    void InvalidateMemory(VAddr addr, u64 size);

    // Ours: how far the backing table covers the GPU-visible guest mappings (the direct and
    // flexible regions that IsValidGpuMapping accepts), for bb-run's log and the tests.
    struct Coverage {
        u64 all_regions = 0;    // the regions in the kernel's table
        u64 regions = 0;        // GPU-visible regions in the kernel's table
        u64 covered = 0;        // of them, those whose every page points at its backing
        u64 pages = 0;          // their pages
        u64 table_pages = 0;    // pages with a backing entry
    };
    Coverage BackingCoverage();

private:
    class Observer;

    AddressSpace impl;
    std::atomic<Vulkan::Rasterizer*> rasterizer{};
    std::unique_ptr<Observer> observer;
};

}  // namespace Core
