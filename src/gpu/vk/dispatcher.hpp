// Records DISPATCH_DIRECT and DISPATCH_INDIRECT on the scheduler (docs/design/graphics.md,
// item 25): the draw block, the compute pipeline, the push constant and the dispatch.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
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
    // recorded is counted and logged (translation errors once per program).
    void dispatch(const pm4::RegisterMirror& regs, const pm4::DispatchCall& call);
    // The parser's sync points: before a write, waits for the recorded work (so that memory
    // packets see what the dispatches wrote); before a wait, submits it.
    void sync(pm4::SyncPoint point);

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
    // A zeroed draw block with the bias table and `code` set; returns its device address and
    // the host pointer through `block`.
    VkDeviceAddress new_block(std::byte*& block);
    void make_chunk();
    static void destroy(VkDevice device, const Chunk& chunk);

    Device& device_;
    Scheduler& scheduler_;
    ComputePipelineCache& pipelines_;
    const MemoryMirror& mirror_;
    Chunk chunk_;
    std::atomic<std::uint64_t> recorded_ = 0, untranslated_ = 0, failed_ = 0;
    std::vector<std::uint64_t> logged_;  // programs whose error was logged
};

}  // namespace bb::vk
