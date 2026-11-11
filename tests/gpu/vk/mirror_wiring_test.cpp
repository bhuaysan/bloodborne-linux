// The memory mirror behind the real vmem and dmem of the HLE: maps, unmaps and releases of direct
// memory reach it through MirrorObserver (the wiring of bb-run, docs/design/graphics.md, item 21).
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "gpu/vk/device.hpp"
#include "gpu/vk/memory_mirror.hpp"
#include "hle/kernel/dmem.hpp"
#include "hle/kernel/vmem.hpp"
#include "test_device.hpp"
#include "tools/mirror_observer.hpp"

namespace {

using namespace bb::hle::kernel;
using bb::vk::Device;
using bb::vk::Instance;
using bb::vk::MemoryMirror;

constexpr std::uint64_t kib = 1024;

TEST(VkMirrorWiring, MapsUnmapsAndReleasesKeepTheMirrorConsistent) {
    const std::string selector = bb::test::vk_test_device();
    std::string why;
    auto instance = Instance::create(why);
    if (!instance) {
        GTEST_SKIP() << "no Vulkan instance: " << why;
    }
    auto device = Device::create(*instance, {.device = selector}, why);
    if (!device) {
        GTEST_SKIP() << "no Vulkan device: " << why;
    }
    ASSERT_GE(dmem::fd(), 0);
    auto mirror = MemoryMirror::create(*device,
                                       {.memfd = dmem::fd(),
                                        .window_start = vmem::window_start,
                                        .size = bb::vk::mirror_buffer_size,
                                        .min_block = 64 * kib},
                                       why);
    if (!mirror) {
        GTEST_SKIP() << "no mirror: " << why;
    }
    bb::tools::MirrorObserver observer(*mirror);
    ASSERT_TRUE(vmem::add_mapping_observer(&observer));
    // Runs before observer and mirror go away, also when an ASSERT below returns early.
    struct ClearObserver {
        bb::tools::MirrorObserver* observer;
        ~ClearObserver() { vmem::remove_mapping_observer(observer); }
    } clear_observer{&observer};

    auto allocate = [] {
        std::uint64_t offset = ~std::uint64_t{0};
        EXPECT_EQ(allocate_direct(0, direct_memory_size(), 256 * kib, 64 * kib, 0, &offset), 0);
        return offset;
    };
    auto map = [](std::uint64_t offset) {
        void* p = nullptr;
        EXPECT_EQ(map_direct(&p, 256 * kib, 3, 0, offset, 64 * kib), 0);
        return p;
    };

    // A map binds, and its unmap unbinds.
    const std::uint64_t first = allocate();
    void* view = map(first);
    EXPECT_EQ(observer.binds(), 1u);
    EXPECT_GT(mirror->imports(), 0u);
    EXPECT_EQ(sceKernelMunmap(view, 256 * kib), 0);
    EXPECT_EQ(observer.unbinds(), 1u);
    EXPECT_EQ(observer.ranges(), 0u);
    EXPECT_EQ(mirror->imports(), 0u);

    // A release while the view is still mapped drops the binding; the unmap after it has nothing
    // left to unbind.
    view = map(first);
    EXPECT_EQ(observer.binds(), 2u);
    EXPECT_EQ(release_direct(first, 256 * kib), 0);
    EXPECT_EQ(observer.releases(), 1u);
    EXPECT_EQ(observer.ranges(), 0u);
    EXPECT_EQ(mirror->imports(), 0u);
    EXPECT_EQ(sceKernelMunmap(view, 256 * kib), 0);
    EXPECT_EQ(observer.unbinds(), 1u);
    EXPECT_EQ(mirror->imports(), 0u);

    // Two views in one 64 KiB block: releasing the memory of one drops the block, and the observer
    // forgets the neighbour's pages in it, so the neighbour's unmap has nothing left to unbind.
    const std::uint64_t shared = allocate();
    void* left = nullptr;
    void* right = nullptr;
    ASSERT_EQ(map_direct(&left, 16 * kib, 3, 0, shared, 16 * kib), 0);
    ASSERT_EQ(map_direct(&right, 16 * kib, 3, 0, shared + 16 * kib, 16 * kib), 0);
    ASSERT_EQ(reinterpret_cast<std::uintptr_t>(left) / (64 * kib),
              reinterpret_cast<std::uintptr_t>(right) / (64 * kib));
    EXPECT_EQ(observer.binds(), 4u);
    EXPECT_EQ(observer.ranges(), 2u);
    EXPECT_EQ(release_direct(shared, 16 * kib), 0);
    EXPECT_EQ(observer.releases(), 2u);
    EXPECT_EQ(observer.ranges(), 0u);
    EXPECT_EQ(mirror->imports(), 0u);
    EXPECT_EQ(sceKernelMunmap(right, 16 * kib), 0);
    EXPECT_EQ(sceKernelMunmap(left, 16 * kib), 0);
    EXPECT_EQ(observer.unbinds(), 1u);
    EXPECT_EQ(release_direct(shared + 16 * kib, 240 * kib), 0);
}

}  // namespace
