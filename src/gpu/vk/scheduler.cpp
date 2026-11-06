#include "gpu/vk/scheduler.hpp"

#include <mutex>
#include <stdexcept>
#include <string>

namespace bb::vk {

Scheduler::Scheduler(Device& device) : device_(device) {
    const VkDevice d = device_.handle();
    auto type = make<VkSemaphoreTypeCreateInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO);
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    auto semaphore_info = make<VkSemaphoreCreateInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                                                      &type);
    check(vkCreateSemaphore(d, &semaphore_info, nullptr, &semaphore_), "vkCreateSemaphore");
    // Beginning a command buffer again resets it.
    auto pool_info = make<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = device_.graphics().family;
    if (VkResult r = vkCreateCommandPool(d, &pool_info, nullptr, &pool_); r != VK_SUCCESS) {
        vkDestroySemaphore(d, semaphore_, nullptr);
        check(r, "vkCreateCommandPool");
    }
}

Scheduler::~Scheduler() {
    // The work being recorded is never submitted, so every deferred function is due afterwards.
    auto info = make<VkSemaphoreWaitInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO);
    info.semaphoreCount = 1;
    info.pSemaphores = &semaphore_;
    info.pValues = &submitted_;
    vkWaitSemaphores(device_.handle(), &info, std::numeric_limits<std::uint64_t>::max());
    run_deferred(std::numeric_limits<std::uint64_t>::max());
    vkDestroyCommandPool(device_.handle(), pool_, nullptr);  // frees the command buffers
    vkDestroySemaphore(device_.handle(), semaphore_, nullptr);
}

VkCommandBuffer Scheduler::commands() {
    if (current_ == none) {
        // A command buffer whose last submission has completed, or a new one.
        const std::uint64_t done = completed();
        std::size_t i = 0;
        while (i < slots_.size() && slots_[i].value > done) {
            ++i;
        }
        if (i == slots_.size()) {
            auto info = make<VkCommandBufferAllocateInfo>(
                VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
            info.commandPool = pool_;
            info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            info.commandBufferCount = 1;
            VkCommandBuffer commands = VK_NULL_HANDLE;
            check(vkAllocateCommandBuffers(device_.handle(), &info, &commands),
                  "vkAllocateCommandBuffers");
            slots_.push_back({commands, 0});
        }
        auto begin = make<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(slots_[i].commands, &begin), "vkBeginCommandBuffer");
        current_ = i;
    }
    return slots_[current_].commands;
}

std::uint64_t Scheduler::flush() {
    if (current_ != none) {
        // On a failure the slot is begun again later, which resets it.
        Slot& slot = slots_[current_];
        current_ = none;
        auto barrier = make<VkMemoryBarrier2>(VK_STRUCTURE_TYPE_MEMORY_BARRIER_2);
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        auto dependency = make<VkDependencyInfo>(VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(slot.commands, &dependency);
        check(vkEndCommandBuffer(slot.commands), "vkEndCommandBuffer");

        auto commands = make<VkCommandBufferSubmitInfo>(
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO);
        commands.commandBuffer = slot.commands;
        auto signal = make<VkSemaphoreSubmitInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO);
        signal.semaphore = semaphore_;
        signal.value = submitted_ + 1;
        signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        auto submit = make<VkSubmitInfo2>(VK_STRUCTURE_TYPE_SUBMIT_INFO_2);
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &commands;
        submit.signalSemaphoreInfoCount = 1;
        submit.pSignalSemaphoreInfos = &signal;
        {
            Queue& queue = device_.graphics();
            const std::lock_guard lock(queue.lock);
            check(vkQueueSubmit2(queue.handle, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit2");
        }
        slot.value = ++submitted_;
    }
    run_deferred(completed());
    return submitted_;
}

std::uint64_t Scheduler::completed() const {
    std::uint64_t value = 0;
    check(vkGetSemaphoreCounterValue(device_.handle(), semaphore_, &value),
          "vkGetSemaphoreCounterValue");
    return value;
}

bool Scheduler::wait(std::uint64_t value, std::uint64_t timeout_ns) {
    if (value > submitted_) {
        throw std::logic_error("wait for value " + std::to_string(value) + ", but only " +
                               std::to_string(submitted_) + " was submitted");
    }
    auto info = make<VkSemaphoreWaitInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO);
    info.semaphoreCount = 1;
    info.pSemaphores = &semaphore_;
    info.pValues = &value;
    const VkResult result = vkWaitSemaphores(device_.handle(), &info, timeout_ns);
    if (result != VK_TIMEOUT) {
        check(result, "vkWaitSemaphores");
    }
    run_deferred(completed());
    return result == VK_SUCCESS;
}

void Scheduler::defer(std::function<void()> function) {
    deferred_.emplace_back(recording() ? submitted_ + 1 : submitted_, std::move(function));
}

void Scheduler::run_deferred(std::uint64_t completed) {
    while (!deferred_.empty() && deferred_.front().first <= completed) {
        // Taken off the queue first, so a function may defer another one.
        auto function = std::move(deferred_.front().second);
        deferred_.pop_front();
        function();
    }
}

}  // namespace bb::vk
