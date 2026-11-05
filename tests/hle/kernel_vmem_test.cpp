#include "hle/kernel/vmem.hpp"

#include <sys/mman.h>

#include <cerrno>
#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include "hle/hle.hpp"

namespace {

using namespace bb::hle::kernel;
using vmem::page_size;

constexpr std::int32_t sce_einval = static_cast<std::int32_t>(0x80020016u);

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

    // libc.prx trims arenas from the inside (0x24f6d): unmap the first two pages, then the last.
    EXPECT_EQ(sceKernelMunmap(ptr(base), 2 * page_size), 0);
    EXPECT_FALSE(vmem::find(base));
    EXPECT_FALSE(vmem::find(base + page_size));
    region = vmem::find(base + 2 * page_size + 1);
    ASSERT_TRUE(region);
    EXPECT_EQ(region->start, base + 2 * page_size);
    EXPECT_EQ(region->prot, 3);
    EXPECT_EQ(sceKernelMunmap(ptr(base + 2 * page_size), page_size), 0);
    EXPECT_FALSE(vmem::find(base + 2 * page_size));
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

TEST(KernelVmem, ImportsResolveToTheImplementations) {
    const auto resolve = [](const char* nid) {
        return bb::hle::resolve({nid, "libkernel", "libkernel", bb::loader::stt_func});
    };
    EXPECT_EQ(resolve("cQke9UuBQOk"), addr(reinterpret_cast<void*>(&sceKernelMunmap)));
    EXPECT_EQ(resolve("vSMAm3cxYTY"), addr(reinterpret_cast<void*>(&sceKernelMprotect)));
}

}  // namespace
