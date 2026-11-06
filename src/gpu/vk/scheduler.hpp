// bbvk's scheduler: records work for the graphics queue and tracks its completion with one
// timeline semaphore (docs/design/graphics.md, "Command buffers to the first frame", item 14).
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

#include <vulkan/vulkan.h>

#include "gpu/vk/device.hpp"

namespace bb::vk {

// Work is recorded into one command buffer at a time. flush() submits it to the graphics queue,
// signalling the semaphore with the next value: 1 for the first submission, then 2, 3 and so on.
// Every submission ends with a barrier from all device writes to host reads, so the host sees
// what the work wrote once its value is reached. A command buffer is reused once the value of
// its last submission is reached.
//
// One thread uses a scheduler. Other threads may wait on semaphore() themselves.
class Scheduler {
public:
    // Throws std::runtime_error when a Vulkan call fails. The device must outlive the scheduler.
    explicit Scheduler(Device& device);
    // Drops the work being recorded, waits for the submitted work and runs every deferred
    // function.
    ~Scheduler();
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    // The command buffer being recorded, begun on the first call after a flush.
    VkCommandBuffer commands();
    // Whether commands() was called since the last flush.
    bool recording() const { return current_ != none; }

    // The value of the last submission, 0 before the first.
    std::uint64_t submitted() const { return submitted_; }

    // Submits the work being recorded and returns its value. With nothing recorded, submits
    // nothing and returns submitted(). Then runs the deferred functions that are due.
    std::uint64_t flush();

    // The value the semaphore has reached.
    std::uint64_t completed() const;
    // Waits until the semaphore reaches `value` or `timeout_ns` passes, then runs the deferred
    // functions that are due. Returns whether the value was reached. Throws std::logic_error
    // for a value above submitted(), which would never be reached.
    bool wait(std::uint64_t value,
              std::uint64_t timeout_ns = std::numeric_limits<std::uint64_t>::max());
    // Submits the work being recorded and waits for all work.
    void finish() { wait(flush()); }

    // Runs `function` once the work recorded so far has completed: the work being recorded if
    // there is any, otherwise the last submission. It runs on the scheduler's thread in the
    // first flush(), wait() or destructor that finds it due, for example to destroy objects that
    // the work uses. Deferred functions run in the order they were deferred.
    void defer(std::function<void()> function);

    VkSemaphore semaphore() const { return semaphore_; }

private:
    struct Slot {
        VkCommandBuffer commands;
        std::uint64_t value;  // of its last submission; 0 before the first
    };

    void run_deferred(std::uint64_t completed);

    Device& device_;
    VkSemaphore semaphore_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    std::vector<Slot> slots_;
    static constexpr std::size_t none = std::numeric_limits<std::size_t>::max();
    std::size_t current_ = none;  // the slot being recorded
    std::uint64_t submitted_ = 0;
    // The functions with the value they wait for, which never decreases along the queue.
    std::deque<std::pair<std::uint64_t, std::function<void()>>> deferred_;
};

}  // namespace bb::vk
