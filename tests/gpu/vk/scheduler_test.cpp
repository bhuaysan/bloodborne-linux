// bbvk's scheduler on lavapipe.

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "gpu/vk/device.hpp"
#include "gpu/vk/scheduler.hpp"

namespace {

using namespace bb::vk;

// A host-visible, coherent buffer that transfers write to.
struct HostBuffer {
    VkDevice device;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    const std::uint32_t* words = nullptr;

    HostBuffer(const Device& d, VkDeviceSize size) : device(d.handle()) {
        auto info = make<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
        info.size = size;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        check(vkCreateBuffer(device, &info, nullptr, &buffer), "vkCreateBuffer");
        VkMemoryRequirements needs;
        vkGetBufferMemoryRequirements(device, buffer, &needs);
        const auto type = d.memory_type(needs.memoryTypeBits,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (!type) {
            throw std::runtime_error("no host-coherent memory for the buffer");
        }
        auto alloc = make<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
        alloc.allocationSize = needs.size;
        alloc.memoryTypeIndex = *type;
        check(vkAllocateMemory(device, &alloc, nullptr, &memory), "vkAllocateMemory");
        check(vkBindBufferMemory(device, buffer, memory, 0), "vkBindBufferMemory");
        void* map = nullptr;
        check(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &map), "vkMapMemory");
        words = static_cast<const std::uint32_t*>(map);
    }
    ~HostBuffer() {
        vkDestroyBuffer(device, buffer, nullptr);
        vkFreeMemory(device, memory, nullptr);
    }
};

class VkScheduler : public testing::Test {
protected:
    void SetUp() override {
        ::setenv("VK_LOADER_DRIVERS_SELECT", "*lvp*", 0);  // as in device_test.cpp
        std::string why;
        instance_ = Instance::create(why);
        if (!instance_) {
            GTEST_SKIP() << "no Vulkan instance: " << why;
        }
        device_ = Device::create(*instance_, {.device = "llvmpipe"}, why);
        if (!device_) {
            GTEST_SKIP() << "no lavapipe device: " << why;
        }
        scheduler_.emplace(*device_);
    }

    // Records a fill of the first `words` words of `b` with `value`.
    void fill(const HostBuffer& b, std::uint32_t words, std::uint32_t value) {
        vkCmdFillBuffer(scheduler_->commands(), b.buffer, 0, 4 * words, value);
    }

    // Destroyed in reverse: the scheduler before the device before the instance.
    std::unique_ptr<Instance> instance_;
    std::unique_ptr<Device> device_;
    std::optional<Scheduler> scheduler_;
};

TEST_F(VkScheduler, NothingRecordedSubmitsNothing) {
    Scheduler& s = *scheduler_;
    EXPECT_FALSE(s.recording());
    EXPECT_EQ(s.flush(), 0u);
    EXPECT_EQ(s.submitted(), 0u);
    EXPECT_EQ(s.completed(), 0u);
    EXPECT_TRUE(s.wait(0));
    s.finish();
    EXPECT_EQ(s.submitted(), 0u);
}

// Each submission signals the next value, and the host sees what it wrote once it is reached.
TEST_F(VkScheduler, SubmissionsSignalIncreasingValues) {
    Scheduler& s = *scheduler_;
    constexpr std::uint32_t words = 16384;
    const HostBuffer b(*device_, 4 * words);
    for (std::uint32_t value = 1; value <= 3; ++value) {
        fill(b, words, 0x1000 + value);
        EXPECT_TRUE(s.recording());
        EXPECT_EQ(s.commands(), s.commands());  // one command buffer until the flush
        EXPECT_EQ(s.flush(), value);
        EXPECT_FALSE(s.recording());
        EXPECT_EQ(s.submitted(), value);
        ASSERT_TRUE(s.wait(value, 30'000'000'000));
        EXPECT_GE(s.completed(), value);
        const std::vector<std::uint32_t> got(b.words, b.words + words);
        EXPECT_EQ(got, std::vector<std::uint32_t>(words, 0x1000 + value));
    }
    EXPECT_EQ(s.flush(), 3u);  // nothing recorded since
}

TEST_F(VkScheduler, RefusesValueNeverSubmitted) {
    Scheduler& s = *scheduler_;
    EXPECT_THROW(s.wait(1), std::logic_error);
    s.commands();
    EXPECT_THROW(s.wait(1), std::logic_error);  // recorded, not submitted
    EXPECT_EQ(s.flush(), 1u);
    EXPECT_TRUE(s.wait(1));
    EXPECT_THROW(s.wait(2), std::logic_error);
}

// A submission held back on the device by an event the host sets keeps its command buffer, its
// value and the functions deferred for it until the event is set.
TEST_F(VkScheduler, ReusesOnlyCompletedCommandBuffers) {
    Scheduler& s = *scheduler_;
    auto event_info = make<VkEventCreateInfo>(VK_STRUCTURE_TYPE_EVENT_CREATE_INFO);
    VkEvent event = VK_NULL_HANDLE;
    check(vkCreateEvent(device_->handle(), &event_info, nullptr, &event), "vkCreateEvent");
    const VkCommandBuffer held = s.commands();
    vkCmdWaitEvents(held, 1, &event, VK_PIPELINE_STAGE_HOST_BIT,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, nullptr, 0, nullptr, 0, nullptr);
    bool ran = false;
    s.defer([&] { ran = true; });
    EXPECT_EQ(s.flush(), 1u);
    const VkCommandBuffer other = s.commands();
    EXPECT_NE(other, held);
    EXPECT_EQ(s.flush(), 2u);
    EXPECT_FALSE(s.wait(1, 1'000'000));  // a millisecond
    EXPECT_EQ(s.completed(), 0u);
    EXPECT_FALSE(ran);

    check(vkSetEvent(device_->handle(), event), "vkSetEvent");
    EXPECT_TRUE(s.wait(2, 30'000'000'000));
    EXPECT_TRUE(ran);
    EXPECT_EQ(s.commands(), held);  // the first one whose submission completed
    s.finish();
    EXPECT_EQ(s.completed(), 3u);
    vkDestroyEvent(device_->handle(), event, nullptr);
}

TEST_F(VkScheduler, DeferredFunctionsRunOnceTheirWorkCompleted) {
    Scheduler& s = *scheduler_;
    std::vector<int> ran;
    s.commands();
    s.defer([&] {
        EXPECT_GE(s.completed(), 1u);
        ran.push_back(1);
    });
    s.defer([&] { ran.push_back(2); });
    EXPECT_TRUE(ran.empty());  // the work is not even submitted
    EXPECT_EQ(s.flush(), 1u);
    s.wait(1);
    EXPECT_EQ(ran, (std::vector{1, 2}));

    // With nothing recorded, a function waits for the last submission, which has completed:
    // the next flush() runs it, even one that submits nothing.
    s.defer([&] { ran.push_back(3); });
    EXPECT_EQ(ran.size(), 2u);
    s.flush();
    EXPECT_EQ(ran, (std::vector{1, 2, 3}));

    // The destructor runs the rest, also those of work it drops.
    s.commands();
    s.defer([&] { ran.push_back(4); });
    scheduler_.reset();
    EXPECT_EQ(ran, (std::vector{1, 2, 3, 4}));
}

// Another thread may wait on the semaphore while the scheduler's thread submits.
TEST_F(VkScheduler, OtherThreadWaitsOnSemaphore) {
    Scheduler& s = *scheduler_;
    std::uint64_t value = 1;
    VkResult result = VK_NOT_READY;
    std::thread waiter([&] {
        auto info = make<VkSemaphoreWaitInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO);
        info.semaphoreCount = 1;
        const VkSemaphore semaphore = s.semaphore();
        info.pSemaphores = &semaphore;
        info.pValues = &value;
        result = vkWaitSemaphores(device_->handle(), &info, 30'000'000'000);
    });
    s.commands();
    s.flush();
    waiter.join();
    EXPECT_EQ(result, VK_SUCCESS);
}

}  // namespace
