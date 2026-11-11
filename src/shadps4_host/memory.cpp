// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/core/memory.cpp, src/core/address_space.cpp @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-11: the guest memory video_core reads and writes, on top
// of our kernel's vmem and dmem (docs/design/renderer.md, "One backing for every page the GPU can
// see", "The mapping observer replaces the mirror's"). The backing is dmem's memfd; the page table
// follows the kernel's mapping observer instead of upstream's own map calls, and SetRasterizer
// replays the mappings made before it. CopySparseMemory and Protect are upstream's;
// IsValidMapping, ClampRangeSize and TryWriteBacking keep upstream's rules on the kernel's guest
// window, region table and the page table. The observer also lifts the write tracking before a
// system call writes guest memory.
// License texts: third_party/shadps4/LICENSES/

#include "core/memory.h"

#include <sys/mman.h>

#include <algorithm>
#include <bit>
#include <cerrno>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "common/assert.h"
#include "common/log.hpp"
#include "common/logging/log.h"
#include "hle/kernel/dmem.hpp"
#include "hle/kernel/vmem.hpp"
#include "shadps4_host/failure.hpp"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

namespace Core {

namespace vmem = bb::hle::kernel::vmem;
namespace dmem = bb::hle::kernel::dmem;

// Keeps the backing table and the rasterizer in step with the kernel's mappings of direct and
// flexible memory. The kernel calls it without its region lock, so the GPU thread, which looks up
// regions (IsValidMapping), can go on while UnmapMemory waits for it.
class MemoryManager::Observer final : public vmem::MappingObserver {
public:
    explicit Observer(MemoryManager& manager) : manager{manager} {}

    void mapped(std::uintptr_t address, std::uint64_t length, std::uint64_t offset) override {
        if (!manager.IsValidGpuMapping(address, length)) {
            return;
        }
        auto& pages = manager.impl.backing_pages;
        u8* const backing = manager.impl.backing_base + offset;
        for (u64 at = 0; at < length; at += GUEST_PAGE_SIZE) {
            pages[(address + at) >> GUEST_PAGE_BITS] = backing + at;
        }
        if (Vulkan::Rasterizer* const r = manager.rasterizer.load(std::memory_order_acquire)) {
            r->RegisterMemory(address, length);
            r->MapMemory(address, length);
        }
    }

    // The caches drop the range first, while its backing is still there for what they write
    // back.
    void unmapped(std::uintptr_t address, std::uint64_t length, std::uint64_t) override {
        if (!manager.IsValidGpuMapping(address, length)) {
            return;
        }
        if (Vulkan::Rasterizer* const r = manager.rasterizer.load(std::memory_order_acquire)) {
            r->UnmapMemory(address, length);
        }
        auto& pages = manager.impl.backing_pages;
        for (u64 at = 0; at < length; at += GUEST_PAGE_SIZE) {
            pages[(address + at) >> GUEST_PAGE_BITS] = nullptr;
        }
    }

    // Upstream unmaps the direct mappings of a released range, the caches first. Ours stay mapped
    // (dmem.cpp, release_direct) with their backing entries, which read zeros once dmem punched
    // the hole; only the caches drop the range. A later unmap tells them again, to no effect.
    void released(std::uint64_t offset, std::uint64_t length) override {
        Vulkan::Rasterizer* const r = manager.rasterizer.load(std::memory_order_acquire);
        vmem::without_changes([&] {
            for (const vmem::Region& g : vmem::regions()) {
                const u64 low = std::max(offset, g.backing);
                const u64 high = std::min(offset + length, g.backing + (g.end - g.start));
                const VAddr address = g.start + (low - g.backing);
                if (r != nullptr && (g.flags & vmem::region_direct) != 0 && low < high &&
                    manager.IsValidGpuMapping(address, high - low)) {
                    r->UnmapMemory(address, high - low);
                }
            }
        });
    }

    // Ours (docs/design/renderer.md, "System calls into tracked memory"): the caches lift their
    // write protection of the GPU-mapped parts of the range, as a fault there would. The parts
    // are collected first, so InvalidateMemory runs without the rasterizer's range lock.
    void host_writing(std::uintptr_t address, std::uint64_t length) override {
        Vulkan::Rasterizer* const r = manager.rasterizer.load(std::memory_order_acquire);
        if (r == nullptr) {
            return;
        }
        std::vector<std::pair<VAddr, VAddr>> parts;
        r->ForEachMappedRangeInRange(address, length, [&](const auto& range) {
            parts.emplace_back(std::max<VAddr>(range.lower(), address),
                               std::min<VAddr>(range.upper(), address + length));
        });
        for (const auto& [low, high] : parts) {
            r->InvalidateMemory(low, high - low);
        }
    }

private:
    MemoryManager& manager;
};

MemoryManager::MemoryManager() : observer{std::make_unique<Observer>(*this)} {
    const int fd = dmem::fd();
    void* const base = fd < 0 ? MAP_FAILED
                              : ::mmap(nullptr, dmem::memfd_size, PROT_READ | PROT_WRITE,
                                       MAP_SHARED | MAP_NORESERVE, fd, 0);
    if (base == MAP_FAILED) {
        bb::common::log_fatal("shadps4", fmt::format("guest memory: no backing of {:#x} bytes: {}",
                                                     dmem::memfd_size, std::strerror(errno)));
    }
    impl.backing_base = static_cast<u8*>(base);
    if (!vmem::add_mapping_observer(observer.get())) {
        bb::common::log_fatal("shadps4", "guest memory: no free mapping observer slot");
    }
}

MemoryManager::~MemoryManager() {
    vmem::remove_mapping_observer(observer.get());
    ::munmap(impl.backing_base, dmem::memfd_size);
}

// Upstream's memory map holds the whole guest address space, free ranges included, so a range is
// valid when it lies in that space. Ours is the guest window, where the kernel places every
// mapping that has no fixed address, and the guest mappings outside it.
bool MemoryManager::IsValidMapping(VAddr virtual_addr, u64 size) {
    const VAddr end = virtual_addr + std::max<u64>(size, 1);
    if (end <= virtual_addr) {
        return false;
    }
    for (VAddr at = virtual_addr; at < end;) {
        if (vmem::in_window(at, at + 1)) {
            at = vmem::window_end;
            continue;
        }
        const std::optional<vmem::Region> region = vmem::find(at);
        if (!region) {
            return false;
        }
        at = region->end;
    }
    return true;
}

void MemoryManager::SetRasterizer(Vulkan::Rasterizer* rasterizer_) {
    // Ours: the rasterizer hears of the GPU-visible mappings made before it, and no map or unmap
    // runs in between.
    vmem::without_changes([&] {
        rasterizer.store(rasterizer_, std::memory_order_release);
        if (rasterizer_ == nullptr) {
            return;
        }
        for (const vmem::Region& r : vmem::regions()) {
            if (r.backing != vmem::no_backing && IsValidGpuMapping(r.start, r.end - r.start)) {
                rasterizer_->RegisterMemory(r.start, r.end - r.start);
                rasterizer_->MapMemory(r.start, r.end - r.start);
            }
        }
    });
}

u64 MemoryManager::ClampRangeSize(VAddr virtual_addr, u64 size) {
    static constexpr u64 MinSizeToClamp = u64{1} << 30;
    // Dont bother with clamping if the size is small so we dont pay a map lookup on every buffer.
    if (size < MinSizeToClamp) {
        return size;
    }

    ASSERT_MSG(IsValidMapping(virtual_addr), "Attempted to access invalid address {:#x}",
               virtual_addr);

    // Clamp size to the remaining size of the current region. Upstream's free ranges are regions
    // of their own; ours end at the next region or the end of the window.
    std::optional<vmem::Region> region = vmem::find(virtual_addr);
    VAddr region_end = region ? region->end : vmem::window_end;
    for (const vmem::Region& r : region ? std::vector<vmem::Region>{} : vmem::regions()) {
        region_end = r.start > virtual_addr ? std::min(region_end, r.start) : region_end;
    }
    u64 clamped_size = region_end - virtual_addr;

    // Keep adding to the size while there is contiguous mapped address space.
    while (clamped_size < size) {
        region = vmem::find(virtual_addr + clamped_size);
        if (!region || region->reserved) {
            break;
        }
        clamped_size += region->end - region->start;
    }
    clamped_size = std::min(clamped_size, size);

    if (size != clamped_size) {
        LOG_DEBUG(Kernel_Vmm, "Clamped requested buffer range addr={:#x}, size={:#x} to {:#x}",
                  virtual_addr, size, clamped_size);
    }
    return clamped_size;
}

void MemoryManager::CopySparseMemory(VAddr virtual_addr, u8* dest, u64 size) {
    const auto& backing_pages = impl.BackingPages();
    while (size) {
        const u64 page = virtual_addr >> GUEST_PAGE_BITS;
        const u64 offset_in_page = virtual_addr % GUEST_PAGE_SIZE;
        const u64 copy_size = std::min<u64>(GUEST_PAGE_SIZE - offset_in_page, size);
        // Ours: the table ends at the GPU's 40 bits, and so do the pages it backs.
        const auto* entry = IsValidGpuMapping(virtual_addr, 0) ? backing_pages.find(page) : nullptr;
        if (entry && *entry) {
            std::memcpy(dest, *entry + offset_in_page, copy_size);
        } else {
            std::memset(dest, 0, copy_size);
        }
        size -= copy_size;
        virtual_addr += copy_size;
        dest += copy_size;
    }
}

// Upstream writes the backed mappings from the start of the range on, up to the first that has
// no backing, and fails if the first has none. Here the same goes page by page. A range that runs
// past its mapping is valid, as upstream's; only its backed start is written.
bool MemoryManager::TryWriteBacking(void* address, const void* data, u64 size) {
    VAddr virtual_addr = std::bit_cast<VAddr>(address);
    ASSERT_MSG(IsValidMapping(virtual_addr, size), "Attempted to access invalid address {:#x}",
               virtual_addr);

    const auto& backing_pages = impl.BackingPages();
    const auto* source = static_cast<const u8*>(data);
    bool wrote = false;
    while (size && IsValidGpuMapping(virtual_addr, 0)) {
        const auto* entry = backing_pages.find(virtual_addr >> GUEST_PAGE_BITS);
        if (!entry || !*entry) {
            break;
        }
        const u64 offset_in_page = virtual_addr % GUEST_PAGE_SIZE;
        const u64 copy_size = std::min<u64>(GUEST_PAGE_SIZE - offset_in_page, size);
        std::memcpy(*entry + offset_in_page, source, copy_size);
        wrote = true;
        size -= copy_size;
        virtual_addr += copy_size;
        source += copy_size;
    }
    return wrote;
}

MemoryManager::Coverage MemoryManager::BackingCoverage() {
    Coverage out;
    // The observer changes the table only under the same serialization.
    vmem::without_changes([&] {
        const std::vector<vmem::Region> all = vmem::regions();
        out.all_regions = all.size();
        for (const vmem::Region& r : all) {
            if (r.backing == vmem::no_backing || !IsValidGpuMapping(r.start, r.end - r.start)) {
                continue;
            }
            ++out.regions;
            bool covered = true;
            for (VAddr at = r.start; at < r.end; at += GUEST_PAGE_SIZE) {
                const auto* entry = impl.backing_pages.find(at >> GUEST_PAGE_BITS);
                covered = covered && entry &&
                          *entry == impl.backing_base + r.backing + (at - r.start);
                ++out.pages;
            }
            out.covered += covered ? 1 : 0;
        }
        // Every page of the GPU's 40 bits, a level-one page at a time.
        constexpr u64 l2_pages = u64{1} << (AddressSpace::Traits::ADDRESS_SPACE_BITS -
                                            AddressSpace::Traits::L1_BITS - GUEST_PAGE_BITS);
        for (u64 l1 = 0; l1 < (u64{1} << AddressSpace::Traits::L1_BITS); ++l1) {
            if (impl.backing_pages.find(l1 * l2_pages) == nullptr) {
                continue;
            }
            for (u64 page = l1 * l2_pages; page < (l1 + 1) * l2_pages; ++page) {
                out.table_pages += *impl.backing_pages.find(page) != nullptr ? 1 : 0;
            }
        }
    });
    return out;
}

void AddressSpace::Protect(VAddr virtual_addr, u64 size, MemoryPermission perms) {
    int flags = PROT_NONE;
    if (True(perms & MemoryPermission::Read)) {
        flags |= PROT_READ;
    }
    if (True(perms & MemoryPermission::Write)) {
        flags |= PROT_WRITE;
    }
    if (True(perms & MemoryPermission::Execute)) {
        flags |= PROT_EXEC;
    }
    int ret = mprotect(reinterpret_cast<void*>(virtual_addr), size, flags);
    ASSERT_MSG(ret == 0, "mprotect failed: {}", std::strerror(errno));
}

// Used only by the userfaultfd tracker, which EmulatorSettings keeps off.
std::vector<AddressSpace::Region> AddressSpace::GetUsableRegions() {
    bb::shadps4::host_missing("Core::AddressSpace::GetUsableRegions (userfaultfd is off)");
}

}  // namespace Core
