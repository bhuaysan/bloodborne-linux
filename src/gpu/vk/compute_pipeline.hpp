// Compute pipelines from the CS state: the translated program in a pipeline with one push
// constant, cached in memory by the state (docs/design/graphics.md, item 25).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>

#include <vulkan/vulkan.h>

#include "gpu/gcn/translate.hpp"
#include "gpu/vk/pipeline_state.hpp"

namespace bb::vk {

class Device;

// The guest code at a guest address: the readable dwords from there on (to the end of its
// mapping), or none when the address isn't readable.
using CodeReader = std::function<std::span<const std::uint32_t>(std::uint64_t address)>;

// The push-constant range of a compute pipeline: the draw block's address (gcn::compute push
// offset 0).
inline constexpr std::uint32_t compute_push_size = 8;

struct ComputePipeline {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;  // no descriptor sets yet (textures item, 28)
    gcn::ShaderInterface interface;
};

class ComputePipelineCache {
public:
    // The device must outlive the cache, and the GPU must be done with the pipelines when it is
    // destroyed. `read` gives the program's code.
    ComputePipelineCache(const Device& device, CodeReader read);
    ~ComputePipelineCache();
    ComputePipelineCache(const ComputePipelineCache&) = delete;
    ComputePipelineCache& operator=(const ComputePipelineCache&) = delete;

    // The pipeline for `state`, made on the first request; an equal state returns the same
    // pipeline. Nothing, with `why` saying what, when the code isn't readable, doesn't translate
    // or uses images or samplers (not yet). A translation error is kept and given again for an
    // equal request, unreadable code is not. Throws std::runtime_error when a Vulkan call
    // fails; nothing is kept then, so the next request for the state tries again.
    const ComputePipeline* get(const ComputeState& state, std::string& why);

    std::size_t size() const { return entries_.size(); }  // pipelines and kept errors
    // For tests: called in the middle of creating a pipeline (after its layout exists); what it
    // throws is a Vulkan failure.
    void set_create_hook(std::function<void()> hook) { create_hook_ = std::move(hook); }

    std::size_t translations() const { return translations_; }

private:
    struct Entry {
        ComputeState state;
        ComputePipeline pipeline;
        std::string error;  // why there is no pipeline
    };

    void destroy(Entry& entry);
    void create(Entry& entry, const gcn::Translation& translation);

    const Device& device_;
    CodeReader read_;
    std::function<void()> create_hook_;
    // By the code address; nodes stay where they are, so handed-out pipelines do too.
    std::unordered_multimap<std::uint64_t, Entry> entries_;
    std::size_t translations_ = 0;
};

}  // namespace bb::vk
