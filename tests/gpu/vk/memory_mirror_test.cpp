// bbvk's MemoryMirror on lavapipe with a 2 GiB mirror and blocks of 64 KiB (RADV's granularity):
// bind, unbind and blocks shared by several bindings. /dev/udmabuf must open, else the tests skip.

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

#include <gtest/gtest.h>

#include "gpu/vk/device.hpp"
#include "gpu/vk/memory_mirror.hpp"
#include "test_device.hpp"
#include "tools/mirror_observer.hpp"

namespace {

using namespace bb::vk;

constexpr std::uint64_t kib = 1024;
constexpr std::uintptr_t window_start = std::uintptr_t{16} << 31;  // a multiple of 2 GiB

// The dword the memfd holds at `offset`.
std::uint32_t pattern(std::uint64_t offset) {
    return 0xb10d0000u + static_cast<std::uint32_t>(offset / 4);
}

class VkMemoryMirror : public ::testing::Test {
protected:
    void SetUp() override {
        const std::string selector = bb::test::vk_test_device();
        std::string why;
        instance_ = Instance::create(why);
        if (!instance_) {
            GTEST_SKIP() << "no Vulkan instance: " << why;
        }
        device_ = Device::create(*instance_, {.device = selector}, why);
        if (!device_) {
            GTEST_SKIP() << "no Vulkan device: " << why;
        }
        // udmabuf needs a memfd sealed against shrinking.
        fd_ = ::memfd_create("mirror-test", MFD_ALLOW_SEALING);
        ASSERT_GE(fd_, 0);
        ASSERT_EQ(::ftruncate(fd_, memfd_size), 0);
        ASSERT_EQ(::fcntl(fd_, F_ADD_SEALS, F_SEAL_SHRINK), 0);
        view_ = static_cast<std::uint32_t*>(
            ::mmap(nullptr, memfd_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0));
        ASSERT_NE(view_, MAP_FAILED);
        for (std::size_t i = 0; i < memfd_size; i += 4) {
            view_[i / 4] = pattern(i);  // each dword tells its offset
        }
        mirror_ = MemoryMirror::create(
            *device_, {.memfd = fd_, .window_start = window_start, .size = mirror_buffer_size,
                       .min_block = 64 * kib},
            why);
        if (!mirror_) {
            GTEST_SKIP() << "no mirror: " << why;
        }
    }

    void TearDown() override {
        mirror_.reset();
        if (view_ != nullptr && view_ != MAP_FAILED) {
            ::munmap(view_, memfd_size);
        }
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    // The dword the GPU reads at guest address `address`: a copy from the sparse buffer.
    std::uint32_t gpu_read(std::uintptr_t address) {
        const VkDevice d = device_->handle();
        Queue& queue = device_->graphics();
        auto buffer_info = make<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
        buffer_info.size = 4;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkBuffer target;
        check(vkCreateBuffer(d, &buffer_info, nullptr, &target), "vkCreateBuffer");
        VkMemoryRequirements needs;
        vkGetBufferMemoryRequirements(d, target, &needs);
        auto alloc = make<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
        alloc.allocationSize = needs.size;
        alloc.memoryTypeIndex = *device_->memory_type(
            needs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkDeviceMemory memory;
        check(vkAllocateMemory(d, &alloc, nullptr, &memory), "vkAllocateMemory");
        check(vkBindBufferMemory(d, target, memory, 0), "vkBindBufferMemory");
        auto pool_info = make<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
        pool_info.queueFamilyIndex = queue.family;
        VkCommandPool pool;
        check(vkCreateCommandPool(d, &pool_info, nullptr, &pool), "vkCreateCommandPool");
        auto cb_info = make<VkCommandBufferAllocateInfo>(
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
        cb_info.commandPool = pool;
        cb_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cb_info.commandBufferCount = 1;
        VkCommandBuffer cb;
        check(vkAllocateCommandBuffers(d, &cb_info, &cb), "vkAllocateCommandBuffers");
        auto begin = make<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
        check(vkBeginCommandBuffer(cb, &begin), "vkBeginCommandBuffer");
        const std::uint64_t window = address - window_start;
        const VkBufferCopy copy{window % mirror_buffer_size, 0, 4};
        vkCmdCopyBuffer(cb, mirror_->buffer(window / mirror_buffer_size), target, 1, &copy);
        check(vkEndCommandBuffer(cb), "vkEndCommandBuffer");
        auto submit = make<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cb;
        {
            const std::scoped_lock lock(queue.lock);
            check(vkQueueSubmit(queue.handle, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");
            check(vkQueueWaitIdle(queue.handle), "vkQueueWaitIdle");
        }
        void* map;
        check(vkMapMemory(d, memory, 0, 4, 0, &map), "vkMapMemory");
        std::uint32_t value;
        std::memcpy(&value, map, 4);
        vkUnmapMemory(d, memory);
        vkDestroyCommandPool(d, pool, nullptr);
        vkDestroyBuffer(d, target, nullptr);
        vkFreeMemory(d, memory, nullptr);
        return value;
    }

    std::unique_ptr<Instance> instance_;
    std::unique_ptr<Device> device_;
    std::unique_ptr<MemoryMirror> mirror_;
    int fd_ = -1;
    std::uint32_t* view_ = nullptr;
    static constexpr std::size_t memfd_size = 1024 * kib;
};

TEST_F(VkMemoryMirror, BindsAndUnbinds) {
    EXPECT_EQ(mirror_->block(), 64 * kib);
    EXPECT_EQ(mirror_->size(), mirror_buffer_size);
    EXPECT_NE(mirror_->bias_table(), 0u);
    // Every bias takes the guest address to buffer 0's address (one buffer: steps repeat it).
    auto address = make<VkBufferDeviceAddressInfo>(VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO);
    address.buffer = mirror_->buffer(0);
    const std::uint64_t bias = vkGetBufferDeviceAddress(device_->handle(), &address) - window_start;
    for (std::size_t e = 0; e < 16; ++e) {
        EXPECT_EQ(mirror_->bias(e), bias) << e;
    }
    // Window offset 0x30000 views memfd offset 0x10000: the GPU reads what the memfd holds.
    const std::uintptr_t at = window_start + 0x30000;
    ASSERT_TRUE(mirror_->bind(at, 64 * kib, 0x10000));
    EXPECT_EQ(mirror_->imports(), 1u);
    EXPECT_EQ(gpu_read(at), pattern(0x10000));
    EXPECT_EQ(gpu_read(at + 0x8004), pattern(0x18004));
    // A CPU write after the bind is seen without a copy.
    view_[0x10000 / 4 + 5] = 0x1234;
    EXPECT_EQ(gpu_read(at + 20), 0x1234u);
    mirror_->unbind(at, 64 * kib, 0x10000);
    EXPECT_EQ(mirror_->imports(), 0u);
    EXPECT_NE(gpu_read(at + 20), 0x1234u);  // the range is gone from the buffer
}

TEST_F(VkMemoryMirror, RejectsBadRanges) {
    EXPECT_FALSE(mirror_->bind(window_start, 0, 0));                         // empty
    EXPECT_FALSE(mirror_->bind(window_start + 0x1000, 16 * kib, 0x1000));    // not a guest page
    EXPECT_FALSE(mirror_->bind(window_start - 16 * kib, 16 * kib, 0));       // before the window
    EXPECT_FALSE(mirror_->bind(window_start + mirror_buffer_size - 16 * kib, 32 * kib, 0));
    EXPECT_FALSE(mirror_->bind(window_start + 0x4000, 16 * kib, 0x8000));    // not congruent
    EXPECT_EQ(mirror_->imports(), 0u);
}

TEST_F(VkMemoryMirror, SharedBlocksCountTheirPages) {
    // Two 16 KiB mappings of one 64 KiB block of the memfd (adjacent views of direct memory).
    const std::uintptr_t block = window_start + 0x20000;
    ASSERT_TRUE(mirror_->bind(block + 0x4000, 16 * kib, 0x24000));
    ASSERT_TRUE(mirror_->bind(block + 0x8000, 16 * kib, 0x28000));
    EXPECT_EQ(mirror_->imports(), 1u);
    EXPECT_EQ(gpu_read(block + 0x8000), pattern(0x28000));
    mirror_->unbind(block + 0x4000, 16 * kib, 0x24000);
    EXPECT_EQ(mirror_->imports(), 1u);  // the other mapping keeps the block
    EXPECT_EQ(gpu_read(block + 0x8000), pattern(0x28000));
    mirror_->unbind(block + 0x8000, 16 * kib, 0x28000);
    EXPECT_EQ(mirror_->imports(), 0u);
}

TEST_F(VkMemoryMirror, BindingTheSameRangeTwiceNeedsTwoUnbinds) {
    const std::uintptr_t at = window_start;
    ASSERT_TRUE(mirror_->bind(at, 64 * kib, 0));
    ASSERT_TRUE(mirror_->bind(at, 64 * kib, 0));
    EXPECT_EQ(mirror_->imports(), 1u);
    mirror_->unbind(at, 64 * kib, 0);
    EXPECT_EQ(mirror_->imports(), 1u);
    EXPECT_EQ(gpu_read(at + 4), pattern(4));
    mirror_->unbind(at, 64 * kib, 0);
    EXPECT_EQ(mirror_->imports(), 0u);
}

TEST_F(VkMemoryMirror, ContiguousBlocksShareAnImportAndFreeItWithTheLast) {
    // Four blocks contiguous in the window and the memfd are one import; a gap makes another.
    ASSERT_TRUE(mirror_->bind(window_start, 256 * kib, 0));
    EXPECT_EQ(mirror_->imports(), 1u);
    ASSERT_TRUE(mirror_->bind(window_start + 0x100000, 64 * kib, 0x80000));
    EXPECT_EQ(mirror_->imports(), 2u);
    EXPECT_EQ(gpu_read(window_start + 0x3fffc), pattern(0x3fffc));
    EXPECT_EQ(gpu_read(window_start + 0x100000), pattern(0x80000));
    mirror_->unbind(window_start, 192 * kib, 0);
    EXPECT_EQ(mirror_->imports(), 2u);  // the import lives while its last block is bound
    EXPECT_EQ(gpu_read(window_start + 0x30000), pattern(0x30000));
    mirror_->unbind(window_start + 0x30000, 64 * kib, 0x30000);
    EXPECT_EQ(mirror_->imports(), 1u);
    mirror_->unbind(window_start + 0x100000, 64 * kib, 0x80000);
    EXPECT_EQ(mirror_->imports(), 0u);
}

TEST_F(VkMemoryMirror, RebindingTheHeadAfterAPartialUnbind) {
    // The first block of an import goes and comes back while the rest stays bound.
    ASSERT_TRUE(mirror_->bind(window_start, 256 * kib, 0));
    mirror_->unbind(window_start, 64 * kib, 0);
    EXPECT_EQ(mirror_->imports(), 1u);
    ASSERT_TRUE(mirror_->bind(window_start, 64 * kib, 0));
    EXPECT_EQ(mirror_->imports(), 2u);
    EXPECT_EQ(gpu_read(window_start + 8), pattern(8));
    EXPECT_EQ(gpu_read(window_start + 0x30008), pattern(0x30008));
    mirror_->unbind(window_start, 256 * kib, 0);
    EXPECT_EQ(mirror_->imports(), 0u);
}

TEST_F(VkMemoryMirror, SplitsARunAtTheImportLimit) {
    const std::uintptr_t at = window_start + mirror_import_limit - 64 * kib;
    ASSERT_TRUE(mirror_->bind(at, 128 * kib, 0x10000));
    EXPECT_EQ(mirror_->imports(), 2u);
    EXPECT_EQ(gpu_read(at + 4), pattern(0x10004));
    EXPECT_EQ(gpu_read(at + 0x10004), pattern(0x20004));
    mirror_->unbind(at, 128 * kib, 0x10000);
    EXPECT_EQ(mirror_->imports(), 0u);
}

TEST_F(VkMemoryMirror, ABlockKeepsItsFirstMemfdBlock) {
    ASSERT_TRUE(mirror_->bind(window_start, 64 * kib, 0));
    ASSERT_TRUE(mirror_->bind(window_start, 64 * kib, 0x10000));  // congruent, another block
    EXPECT_EQ(mirror_->imports(), 1u);
    EXPECT_EQ(gpu_read(window_start + 4), pattern(4));
    mirror_->unbind(window_start, 64 * kib, 0x10000);  // not its view: changes nothing
    EXPECT_EQ(mirror_->imports(), 1u);
    mirror_->unbind(window_start, 64 * kib, 0);
    EXPECT_EQ(mirror_->imports(), 0u);
}

TEST_F(VkMemoryMirror, AFailedBindLeavesNothingBehind) {
    // The second block is past the memfd's end, so its import fails after the first one's.
    const std::uintptr_t at = window_start + 0x100000;
    ASSERT_TRUE(mirror_->bind(at - 64 * kib, 64 * kib, 0xf0000));
    // A shared block that the failing bind would count.
    EXPECT_THROW(mirror_->bind(at - 64 * kib, 192 * kib, 0xf0000), std::runtime_error);
    EXPECT_EQ(mirror_->imports(), 1u);
    mirror_->unbind(at - 64 * kib, 64 * kib, 0xf0000);  // one unbind frees it: no count leaked
    EXPECT_EQ(mirror_->imports(), 0u);
}

TEST_F(VkMemoryMirror, AFailedBindUnbindsTheRunsBoundBeforeIt) {
    // The import limit splits the range in two runs: the first imports, the second is past
    // the memfd's end and fails, so the first has to be rolled back.
    const std::uintptr_t at = window_start + mirror_import_limit - 64 * kib;
    EXPECT_THROW(mirror_->bind(at, 128 * kib, 0xf0000), std::runtime_error);
    EXPECT_EQ(mirror_->imports(), 0u);
    ASSERT_TRUE(mirror_->bind(at, 64 * kib, 0xf0000));  // the block is free to bind again
    EXPECT_EQ(mirror_->imports(), 1u);
    EXPECT_EQ(gpu_read(at + 4), pattern(0xf0004));
    mirror_->unbind(at, 64 * kib, 0xf0000);
    EXPECT_EQ(mirror_->imports(), 0u);
}

TEST_F(VkMemoryMirror, ReleaseUnbindsTheRangeAndRebindsTheRestOfItsImports) {
    // One import of four blocks; releasing the third takes the import, the other blocks return.
    ASSERT_TRUE(mirror_->bind(window_start, 256 * kib, 0));
    EXPECT_EQ(mirror_->imports(), 1u);
    mirror_->release(0x20000, 64 * kib);
    EXPECT_EQ(mirror_->imports(), 2u);  // blocks 0-1 and block 3
    EXPECT_EQ(gpu_read(window_start + 0x1fff8), pattern(0x1fff8));
    EXPECT_EQ(gpu_read(window_start + 0x30008), pattern(0x30008));
    EXPECT_NE(gpu_read(window_start + 0x20008), pattern(0x20008));  // gone from the buffer
    mirror_->unbind(window_start, 128 * kib, 0);
    mirror_->unbind(window_start + 0x30000, 64 * kib, 0x30000);
    EXPECT_EQ(mirror_->imports(), 0u);
    mirror_->release(0, 64 * kib);  // nothing bound: nothing happens
    EXPECT_EQ(mirror_->imports(), 0u);
}

TEST_F(VkMemoryMirror, TheObserverFollowsMapsAndUnmaps) {
    bb::tools::MirrorObserver observer(*mirror_);
    const std::uintptr_t at = window_start + 0x40000;
    observer.mapped(at, 192 * kib, 0x40000);
    EXPECT_EQ(observer.binds(), 1u);
    EXPECT_EQ(gpu_read(at + 0x10008), pattern(0x50008));
    // Unmapping the middle leaves the two ends bound, each in its own record.
    observer.unmapped(at + 0x10000, 64 * kib, 0x50000);
    EXPECT_EQ(observer.unbinds(), 1u);
    EXPECT_EQ(observer.ranges(), 2u);
    EXPECT_EQ(mirror_->imports(), 1u);  // one import, two blocks left
    EXPECT_EQ(gpu_read(at + 8), pattern(0x40008));
    EXPECT_EQ(gpu_read(at + 0x20008), pattern(0x60008));
    EXPECT_NE(gpu_read(at + 0x10008), pattern(0x50008));
    // A range the mirror refuses is not recorded and not unbound.
    observer.mapped(window_start + mirror_buffer_size, 64 * kib, 0);
    EXPECT_EQ(observer.binds(), 1u);
    EXPECT_EQ(observer.ranges(), 2u);
    observer.unmapped(window_start + mirror_buffer_size, 64 * kib, 0);
    EXPECT_EQ(observer.unbinds(), 1u);
    observer.unmapped(at, 192 * kib, 0x40000);  // the parts that is bound
    observer.unmapped(at, 192 * kib, 0x40000);  // twice changes nothing
    EXPECT_EQ(observer.unbinds(), 3u);
    EXPECT_EQ(observer.ranges(), 0u);
    EXPECT_EQ(mirror_->imports(), 0u);
}

TEST_F(VkMemoryMirror, TheObserverForgetsWhatReleaseTook) {
    bb::tools::MirrorObserver observer(*mirror_);
    observer.mapped(window_start, 128 * kib, 0);
    observer.mapped(window_start + 0x100000, 64 * kib, 0x80000);
    observer.released(0x10000, 64 * kib);  // the second block of the first mapping
    EXPECT_EQ(observer.releases(), 1u);
    EXPECT_EQ(observer.ranges(), 2u);  // the first block and the second mapping
    EXPECT_EQ(mirror_->imports(), 2u);
    // The unmap of the whole first mapping unbinds only what is still bound.
    observer.unmapped(window_start, 128 * kib, 0);
    EXPECT_EQ(observer.unbinds(), 1u);
    EXPECT_EQ(mirror_->imports(), 1u);
    observer.unmapped(window_start + 0x100000, 64 * kib, 0x80000);
    EXPECT_EQ(mirror_->imports(), 0u);
}

// Two mappings that share a 64 KiB block: releasing the memfd range of one drops the block, and
// the observer forgets the neighbour's pages in it, so its late unmap unbinds nothing there.
TEST_F(VkMemoryMirror, TheObserverForgetsASharedBlockOfANeighbour) {
    bb::tools::MirrorObserver observer(*mirror_);
    observer.mapped(window_start, 0x4000, 0);
    observer.mapped(window_start + 0x4000, 0x4000, 0x4000);
    EXPECT_EQ(observer.ranges(), 2u);
    observer.released(0, 0x4000);
    EXPECT_EQ(observer.releases(), 1u);
    EXPECT_EQ(observer.ranges(), 0u);  // the neighbour went with the shared block
    observer.unmapped(window_start + 0x4000, 0x4000, 0x4000);
    EXPECT_EQ(observer.unbinds(), 0u);
    EXPECT_EQ(mirror_->imports(), 0u);
}

}  // namespace
