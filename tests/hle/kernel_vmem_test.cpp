#include "hle/kernel/vmem.hpp"

#include <sys/mman.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>

#include <gtest/gtest.h>

#include "hle/hle.hpp"
#include "hle/kernel/abi.hpp"

namespace {

using namespace bb::hle::kernel;
using vmem::page_size;

constexpr std::int32_t sce_einval = static_cast<std::int32_t>(0x80020016u);
constexpr int guest_map_private = 0x2;
constexpr int guest_map_fixed = 0x10;
constexpr int guest_map_anon = 0x1000;
void* const map_failed = reinterpret_cast<void*>(-1);

std::uintptr_t addr(const void* p) {
    return reinterpret_cast<std::uintptr_t>(p);
}

void* ptr(std::uintptr_t a) {
    return reinterpret_cast<void*>(a);
}

// An anonymous private mapping, as the map calls of the eboot and libc.prx make them.
vmem::Mapping anonymous(std::uint64_t pages, int prot = 3) {
    vmem::Mapping m;
    m.length = pages * page_size;
    m.prot = prot;
    m.host_flags = MAP_PRIVATE | MAP_ANONYMOUS;
    return m;
}

std::uintptr_t map_pages(std::uint64_t pages, int prot = 3) {
    std::uintptr_t out = 0;
    EXPECT_EQ(vmem::map(anonymous(pages, prot), out), 0);
    return out;
}

// The guest errno after a POSIX call; resets it.
int take_errno() {
    const int e = *guest_errno_location();
    *guest_errno_location() = 0;
    return e;
}

TEST(KernelVmem, MapsAlignedAndRecordsTheRegion) {
    vmem::Mapping m = anonymous(4);
    m.name = "arena";
    std::uintptr_t p = 0;
    ASSERT_EQ(vmem::map(m, p), 0);
    EXPECT_EQ(p % page_size, 0U);
    static_cast<char*>(ptr(p))[4 * page_size - 1] = 1;
    const auto region = vmem::find(p + 4 * page_size - 1);
    ASSERT_TRUE(region);
    EXPECT_EQ(region->start, p);
    EXPECT_EQ(region->end, p + 4 * page_size);
    EXPECT_EQ(region->prot, 3);
    EXPECT_EQ(region->flags, vmem::region_anonymous);
    EXPECT_EQ(region->name, "arena");
    EXPECT_FALSE(vmem::find(p + 4 * page_size));

    EXPECT_EQ(sceKernelMunmap(ptr(p), 4 * page_size), 0);
    EXPECT_FALSE(vmem::find(p));
    EXPECT_EQ(sceKernelMunmap(ptr(p), 4 * page_size), sce_einval);
}

TEST(KernelVmem, MapRefusesBadLengthsAndProtection) {
    std::uintptr_t out = 0;
    EXPECT_EQ(vmem::map(anonymous(0), out), EINVAL);
    vmem::Mapping m = anonymous(1);
    m.length = page_size + 0x1000;  // not a whole guest page
    EXPECT_EQ(vmem::map(m, out), EINVAL);
    m.length = std::uint64_t{1} << 48;  // beyond the address space
    EXPECT_EQ(vmem::map(m, out), EINVAL);
    EXPECT_EQ(vmem::map(anonymous(1, 0x43), out), EINVAL);  // unknown protection bit
    EXPECT_EQ(vmem::map(anonymous(1, 0x83), out), EINVAL);
    EXPECT_EQ(out, 0U);
}

TEST(KernelVmem, FixedMapNeverReplacesAMapping) {
    const std::uintptr_t p = map_pages(1);
    vmem::Mapping m = anonymous(1);
    m.fixed = true;
    m.address = p;
    std::uintptr_t out = 0;
    EXPECT_EQ(vmem::map(m, out), EEXIST);
    m.address = p + 0x1000;
    EXPECT_EQ(vmem::map(m, out), EINVAL);
    ASSERT_EQ(vmem::unmap(p, page_size), 0);
    m.address = p;
    ASSERT_EQ(vmem::map(m, out), 0);
    EXPECT_EQ(out, p);
    EXPECT_EQ(vmem::unmap(p, page_size), 0);
}

TEST(KernelVmem, ProtectAndUnmapSplitRegions) {
    const std::uintptr_t base = map_pages(3);

    // Read-only middle page: the table splits into three regions.
    EXPECT_EQ(sceKernelMprotect(ptr(base + page_size), page_size, 1), 0);
    auto region = vmem::find(base + page_size + 8);
    ASSERT_TRUE(region);
    EXPECT_EQ(region->start, base + page_size);
    EXPECT_EQ(region->end, base + 2 * page_size);
    EXPECT_EQ(region->prot, 1);
    region = vmem::find(base);
    ASSERT_TRUE(region);
    EXPECT_EQ(region->end, base + page_size);
    EXPECT_EQ(region->prot, 3);
    static_cast<char*>(ptr(base))[0] = 1;  // still writable

    // libc.prx releases the tail of an arena segment (0x24f6d): munmap(base + size - n, n), n a
    // multiple of its granularity. Here the tail spans two regions, and the head stays.
    EXPECT_EQ(sceKernelMunmap(ptr(base + page_size), 2 * page_size), 0);
    EXPECT_FALSE(vmem::find(base + page_size));
    EXPECT_FALSE(vmem::find(base + 2 * page_size));
    region = vmem::find(base);
    ASSERT_TRUE(region);
    EXPECT_EQ(region->start, base);
    EXPECT_EQ(region->end, base + page_size);
    EXPECT_EQ(region->prot, 3);
    EXPECT_EQ(sceKernelMunmap(ptr(base), page_size), 0);
    EXPECT_FALSE(vmem::find(base));
}

TEST(KernelVmem, DirectRegionsKeepTheirOffsetWhenSplit) {
    vmem::Mapping m = anonymous(2);
    m.flags = vmem::region_direct;
    m.offset = 0x100000;
    std::uintptr_t p = 0;
    ASSERT_EQ(vmem::map(m, p), 0);
    EXPECT_EQ(vmem::protect(p + page_size, page_size, 1), 0);
    const auto region = vmem::find(p + page_size);
    ASSERT_TRUE(region);
    EXPECT_EQ(region->offset, 0x100000 + page_size);
    EXPECT_EQ(region->flags, vmem::region_direct);
    EXPECT_EQ(vmem::unmap(p, 2 * page_size), 0);
}

TEST(KernelVmem, SceCallsRefuseBadRanges) {
    const std::uintptr_t p = map_pages(2);
    EXPECT_EQ(sceKernelMprotect(ptr(p), page_size, 0x40), sce_einval);  // unknown protection
    // Unaligned, empty, or reaching past the mapping: nothing is unmapped.
    EXPECT_EQ(sceKernelMunmap(ptr(p + 0x1000), page_size), sce_einval);
    EXPECT_EQ(sceKernelMunmap(ptr(p), 0), sce_einval);
    EXPECT_EQ(sceKernelMunmap(ptr(p), 3 * page_size), sce_einval);
    EXPECT_EQ(sceKernelMprotect(ptr(p), 3 * page_size, 1), sce_einval);
    EXPECT_EQ(sceKernelMunmap(nullptr, page_size), sce_einval);  // libc.prx 0x23ee9 after a failure
    EXPECT_TRUE(vmem::find(p));
    EXPECT_EQ(sceKernelMunmap(ptr(p), 2 * page_size), 0);
}

TEST(KernelVmem, RuntimeMemoryIsNeverUnmapped) {
    alignas(0x4000) static char runtime_page[0x4000];
    EXPECT_EQ(sceKernelMunmap(runtime_page, sizeof runtime_page), sce_einval);
    EXPECT_EQ(sceKernelMprotect(runtime_page, sizeof runtime_page, 1), sce_einval);
    runtime_page[0] = 1;  // still mapped and writable
}

TEST(KernelVmem, ExtraProtectionBitsAreAcceptedAndLoggedOnce) {
    // The eboot can map its heap with prot 0x33 (0x2081da6).
    testing::internal::CaptureStderr();
    const std::uintptr_t a = map_pages(1, 0x33);
    const std::uintptr_t b = map_pages(1, 0x33);
    const std::string log = testing::internal::GetCapturedStderr();
    ASSERT_NE(a, 0U);
    ASSERT_NE(b, 0U);
    const std::string line = "protection bits 0x30 accepted and not applied";
    const auto first = log.find(line);
    EXPECT_NE(first, std::string::npos) << log;
    EXPECT_EQ(log.find(line, first + 1), std::string::npos) << log;

    static_cast<char*>(ptr(a))[0] = 1;  // read and write applied
    const auto region = vmem::find(a);
    ASSERT_TRUE(region);
    EXPECT_EQ(region->prot, 0x33);  // kept for the protection query (0x26bae04 tests bit 4)
    EXPECT_EQ(sceKernelMprotect(ptr(b), page_size, 0x31), 0);
    EXPECT_EQ(sceKernelMunmap(ptr(a), page_size), 0);
    EXPECT_EQ(sceKernelMunmap(ptr(b), page_size), 0);
}

TEST(KernelVmem, PageSizeIsTheGuestPage) {
    EXPECT_EQ(guest_getpagesize(), 0x4000);
}

TEST(KernelVmem, MmapMunmapRoundTrip) {
    // libc.prx's anonymous mappings (0x52f9b): prot 3, flags 0x1002, fd -1.
    void* p = guest_mmap(nullptr, 0x5000, 3, guest_map_private | guest_map_anon, -1, 0);
    ASSERT_NE(p, map_failed);
    EXPECT_EQ(addr(p) % page_size, 0U);
    std::memset(p, 0xab, 0x8000);  // rounded up to two guest pages
    const auto region = vmem::find(addr(p) + 0x7fff);
    ASSERT_TRUE(region);
    EXPECT_EQ(region->start, addr(p));
    EXPECT_EQ(region->end, addr(p) + 0x8000);
    EXPECT_EQ(region->prot, 3);
    EXPECT_EQ(region->flags, vmem::region_anonymous);

    // A guest MAP_FIXED never replaces a mapping.
    const int fixed = guest_map_private | guest_map_anon | guest_map_fixed;
    EXPECT_EQ(guest_mmap(p, page_size, 3, fixed, -1, 0), map_failed);
    EXPECT_EQ(take_errno(), 17);

    EXPECT_EQ(guest_munmap(p, 0x8000), 0);
    EXPECT_FALSE(vmem::find(addr(p)));
    EXPECT_EQ(guest_munmap(p, 0x8000), -1);
    EXPECT_EQ(take_errno(), 22);
    EXPECT_EQ(guest_mmap(p, page_size, 3, fixed, -1, 0), p);
    EXPECT_EQ(guest_munmap(p, page_size), 0);
}

TEST(KernelVmem, MmapRefusesBadLengthsAndFlags) {
    struct Case {
        std::uint64_t length;
        int prot, flags, errno_value;
    };
    const Case cases[] = {
        {0, 3, 0x1002, 22},                       // empty
        {std::uint64_t{1} << 48, 3, 0x1002, 22},  // beyond the address space
        {0x4000, 3, 0x1002 | 0x20, 22},           // MAP_RENAME: no host counterpart
        {0x4000, 3, 0x1003, 22},                  // shared and private
        {0x4000, 0x43, 0x1002, 22},               // unknown protection bit
        {0x4000, 1, 0x2, 9},                      // a file: no guest descriptors yet
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.flags);
        EXPECT_EQ(guest_mmap(nullptr, c.length, c.prot, c.flags, -1, 0), map_failed);
        EXPECT_EQ(take_errno(), c.errno_value);
    }
}

TEST(KernelVmem, NamedMapShowsUpInTheRegionTable) {
    // libc.prx 0x23eaf: &addr (0), length, prot 3, flags 0, name.
    void* p = nullptr;
    ASSERT_EQ(map_named_anonymous(&p, 0x10000, 3, 0, "SceLibcArena"), 0);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(addr(p) % page_size, 0U);
    bool listed = false;
    for (const vmem::Region& r : vmem::regions()) {
        listed |= r.start == addr(p) && r.end == addr(p) + 0x10000 && r.name == "SceLibcArena";
    }
    EXPECT_TRUE(listed);
    EXPECT_EQ(sceKernelMlock(p, 0x10000), 0);
    EXPECT_EQ(sceKernelMunmap(p, 0x10000), 0);
    EXPECT_FALSE(vmem::find(addr(p)));
    EXPECT_NE(sceKernelMlock(p, 0x10000), 0);
}

TEST(KernelVmem, ProtectAndQueryRoundTrip) {
    void* p = nullptr;
    ASSERT_EQ(map_anonymous(&p, 3 * page_size, 0x33, 0), 0);  // eboot 0x2081da6
    const auto base = addr(p);

    // Read-only middle page: the table splits into three regions.
    EXPECT_EQ(sceKernelMprotect(ptr(base + page_size), page_size, 1), 0);
    std::uint64_t start = 0, end = 0;
    std::uint32_t prot = 0;
    ASSERT_EQ(mapping_protection(ptr(base + page_size + 8), &start, &end, &prot), 0);
    EXPECT_EQ(start, base + page_size);
    EXPECT_EQ(end, base + 2 * page_size);
    EXPECT_EQ(prot, 1U);
    ASSERT_EQ(mapping_protection(p, nullptr, nullptr, &prot), 0);
    EXPECT_EQ(prot, 0x33U);  // 0x26bae04 tests bit 4

    // The eboot's query (0x20825e5): flags 0, a 0x48-byte record.
    QueryRecord record;
    std::memset(&record, 0xff, sizeof record);
    ASSERT_EQ(query_mapping(ptr(base + 2 * page_size + 1), 0, &record, sizeof record), 0);
    EXPECT_EQ(record.start, base + 2 * page_size);
    EXPECT_EQ(record.end, base + 3 * page_size);
    EXPECT_EQ(record.offset, 0U);
    EXPECT_EQ(record.flags, vmem::region_anonymous);
    EXPECT_EQ(record.reserved2[0], 0);

    // Release the first two pages, then the last one.
    EXPECT_EQ(release_anonymous(p, 2 * page_size), 0);
    EXPECT_NE(query_mapping(p, 0, &record, sizeof record), 0);
    EXPECT_NE(mapping_protection(p, nullptr, nullptr, &prot), 0);
    EXPECT_TRUE(vmem::find(base + 2 * page_size));
    EXPECT_EQ(release_anonymous(ptr(base + 2 * page_size), page_size), 0);
    EXPECT_FALSE(vmem::find(base + 2 * page_size));
}

TEST(KernelVmem, AnonymousMapAndQueryRefuseBadArguments) {
    void* p = nullptr;
    EXPECT_EQ(map_anonymous(&p, 0, 3, 0), sce_einval);
    EXPECT_EQ(map_anonymous(&p, page_size + 0x1000, 3, 0), sce_einval);  // not a whole page
    EXPECT_EQ(map_anonymous(&p, page_size, 3, 0x10), sce_einval);         // flags other than 0
    EXPECT_EQ(map_anonymous(&p, page_size, 0x83, 0), sce_einval);         // unknown protection
    EXPECT_EQ(map_anonymous(nullptr, page_size, 3, 0), sce_einval);
    EXPECT_EQ(p, nullptr);

    ASSERT_EQ(map_anonymous(&p, page_size, 3, 0), 0);
    QueryRecord record;
    EXPECT_EQ(query_mapping(p, 0, &record, 0x40), sce_einval);
    EXPECT_EQ(query_mapping(p, 1, &record, sizeof record), sce_einval);
    EXPECT_EQ(release_anonymous(p, page_size), 0);
}

TEST(KernelVmem, MsyncProbesHostPages) {
    // libc.prx 0x541a5 probes a 4 KiB page with MS_ASYNC.
    alignas(0x1000) static char page[0x1000];
    EXPECT_EQ(guest_msync(page, sizeof page, 1), 0);
    EXPECT_EQ(guest_msync(page, sizeof page, 0), 0);  // MS_SYNC
    EXPECT_EQ(guest_msync(page, sizeof page, 2), 0);  // MS_INVALIDATE
    void* p = nullptr;
    ASSERT_EQ(map_anonymous(&p, page_size, 3, 0), 0);
    ASSERT_EQ(sceKernelMunmap(p, page_size), 0);
    EXPECT_EQ(guest_msync(p, 0x1000, 1), -1);
    EXPECT_EQ(take_errno(), 12);
    EXPECT_EQ(guest_msync(page, sizeof page, 4), -1);
    EXPECT_EQ(take_errno(), 22);
    EXPECT_EQ(guest_msync(page, sizeof page, 3), -1);  // MS_ASYNC | MS_INVALIDATE
    EXPECT_EQ(take_errno(), 22);
}

TEST(KernelVmem, DirectMemoryTypeFailsWithoutDirectMemory) {
    std::int32_t type = 7;
    std::uint64_t start = 0, end = 0;
    EXPECT_NE(direct_memory_type(0, &type, &start, &end), 0);
    EXPECT_EQ(type, 7);
}

TEST(KernelVmem, ImportsResolveToTheImplementations) {
    const auto resolve = [](const char* library, const char* nid) {
        return bb::hle::resolve({nid, library, "libkernel", bb::loader::stt_func});
    };
    EXPECT_EQ(resolve("libkernel", "cQke9UuBQOk"), addr(reinterpret_cast<void*>(&sceKernelMunmap)));
    EXPECT_EQ(resolve("libkernel", "vSMAm3cxYTY"),
              addr(reinterpret_cast<void*>(&sceKernelMprotect)));
    EXPECT_EQ(resolve("libkernel", "BPE9s9vQQXo"), addr(reinterpret_cast<void*>(&guest_mmap)));
    EXPECT_EQ(resolve("libScePosix", "BPE9s9vQQXo"), addr(reinterpret_cast<void*>(&guest_mmap)));
    EXPECT_EQ(resolve("libScePosix", "k+AXqu2-eBc"),
              addr(reinterpret_cast<void*>(&guest_getpagesize)));
    EXPECT_EQ(resolve("libkernel", "rVjRvHJ0X6c"), addr(reinterpret_cast<void*>(&query_mapping)));
}

}  // namespace
