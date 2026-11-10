// The interface video_core expects from shadPS4's memory manager (core/memory.h and
// core/address_space.h of v.0.21.0), declared only. #430 implements it on top of vmem and dmem
// (docs/design/renderer.md, "One backing for every page the GPU can see"). Until then every member
// that is not defined here ends the process (video_core_host.cpp), so nothing reaches guest memory
// through a stub that pretends to work.
#pragma once

#include <vector>

#include "common/enum.h"
#include "common/multi_level_page_table.h"
#include "common/singleton.h"
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

    // One mapped range, as the userfaultfd tracker registers them.
    struct Region {
        VAddr base, end;
        VAddr lower() const { return base; }
        VAddr upper() const { return end; }
    };

    const BackingPageTable& BackingPages() const;
    void Protect(VAddr virtual_addr, u64 size, MemoryPermission perms);
    // Used only by the userfaultfd tracker, which EmulatorSettings keeps off.
    std::vector<Region> GetUsableRegions();
};

class MemoryManager {
public:
    // The rasterizer registers itself when it is created.
    void SetRasterizer(Vulkan::Rasterizer* rasterizer_) { rasterizer = rasterizer_; }

    AddressSpace& GetAddressSpace();

    bool IsValidGpuMapping(VAddr virtual_addr, u64 size);
    bool IsValidMapping(VAddr virtual_addr, u64 size = 0);
    u64 ClampRangeSize(VAddr virtual_addr, u64 size);
    void CopySparseMemory(VAddr source, u8* dest, u64 size);
    bool TryWriteBacking(void* address, const void* data, u64 size);

private:
    Vulkan::Rasterizer* rasterizer{};
};

using Memory = Common::Singleton<MemoryManager>;

}  // namespace Core
