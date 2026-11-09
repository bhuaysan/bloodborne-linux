// Records DISPATCH_DIRECT and DISPATCH_INDIRECT on the scheduler (docs/design/graphics.md,
// item 25): the draw block, the compute pipeline, the push constant and the dispatch.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "gpu/pm4/parser.hpp"
#include "gpu/vk/compute_pipeline.hpp"

namespace bb::vk {

class Device;
class MemoryMirror;
class Scheduler;

// Dispatches by outcome: recorded on the scheduler, not recorded because the shader doesn't
// translate (or has no code or program), or not recorded because of an error (a Vulkan failure,
// indirect counts outside the mirror or unaligned, group counts over the device limit).
struct DispatchStats {
    std::uint64_t recorded = 0;
    std::uint64_t untranslated = 0;
    std::uint64_t failed = 0;
};

// One recorded dispatch, for tying a GPU fault to a program (docs/design/graphics.md, "First-frame
// gaps"). The fault's device address gives the guest address through the mirror's map, which
// MemoryMirror::create logs.
struct RecentDispatch {
    std::uint64_t number = 0;   // counts the recorded dispatches from 1
    std::uint64_t program = 0;  // guest address of the code
    VkDeviceAddress block = 0;  // the draw block
    std::uint64_t indirect = 0;  // guest address of the group counts, or 0 for `groups`
    std::array<std::uint32_t, 3> groups{};
    std::array<std::uint32_t, pm4::user_data_dwords> user{};
};

// One log line's text for `dispatch`.
std::string describe(const RecentDispatch& dispatch);

// How many recorded dispatches the Dispatcher keeps for the first Vulkan error.
inline constexpr std::size_t recent_dispatches = 32;

// Used by one thread (the GPU thread), like the scheduler. The device, scheduler, pipeline cache
// and mirror must outlive it.
class Dispatcher {
public:
    Dispatcher(Device& device, Scheduler& scheduler, ComputePipelineCache& pipelines,
               const MemoryMirror& mirror);
    // Waits for the scheduler's work, then frees the draw block memory.
    ~Dispatcher();
    Dispatcher(const Dispatcher&) = delete;
    Dispatcher& operator=(const Dispatcher&) = delete;

    // Records one dispatch with the CS registers `regs`. Never throws: a dispatch that can't be
    // recorded is counted and logged (translation errors once per program). The first recording
    // of a program is logged at info, every recording at trace. Copying it to the list of recent
    // dispatches is all that the other levels pay; the first Vulkan error logs that list.
    void dispatch(const pm4::RegisterMirror& regs, const pm4::DispatchCall& call);
    // The parser's sync points: before a write, waits for the recorded work (so that memory
    // packets see what the dispatches wrote); before a wait, submits it.
    void sync(pm4::SyncPoint point);

    // A draw block for a stage with user data `user` and the program at `code`: the bias table
    // set, the rest zero. Returns its device address. The block lives until the work recorded so
    // far has completed (also for DrawRecorder, which shares the blocks).
    VkDeviceAddress draw_block(std::span<const std::uint32_t, pm4::user_data_dwords> user,
                               std::uint64_t code);

    // The last recent_dispatches recorded dispatches, oldest first.
    std::vector<RecentDispatch> recent() const;

    // A snapshot; another thread may read it while the GPU thread dispatches.
    DispatchStats stats() const {
        return {recorded_.load(), untranslated_.load(), failed_.load()};
    }

private:
    // Draw blocks come from chunks of host-coherent memory, one block per 256 bytes.
    struct Chunk {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        std::byte* map = nullptr;
        VkDeviceAddress address = 0;
        std::uint32_t used = 0;  // blocks
    };

    void record(const pm4::RegisterMirror& regs, const pm4::DispatchCall& call);
    // A zeroed block; returns its device address and the host pointer through `block`.
    VkDeviceAddress new_block(std::byte*& block);
    void make_chunk();
    static void destroy(VkDevice device, const Chunk& chunk);
    // Keeps `dispatch` for recent() and logs it as dispatch() says.
    void remember(const RecentDispatch& dispatch);
    // After a failure: on the first Vulkan error, logs the recent dispatches.
    void report(const std::exception& error);

    Device& device_;
    Scheduler& scheduler_;
    ComputePipelineCache& pipelines_;
    const MemoryMirror& mirror_;
    Chunk chunk_;
    std::atomic<std::uint64_t> recorded_ = 0, untranslated_ = 0, failed_ = 0;
    std::vector<std::uint64_t> logged_;  // programs whose error was logged
    std::array<RecentDispatch, recent_dispatches> recent_{};  // a ring, by number
    std::uint64_t remembered_ = 0;
    std::vector<std::uint64_t> seen_;  // programs whose first recording was logged
    bool reported_ = false;            // the recent dispatches were logged
};

}  // namespace bb::vk
