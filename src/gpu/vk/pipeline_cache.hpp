// Graphics pipelines from the draw snapshot: the translated stages, dynamic rendering into the
// targets' formats and the dynamic state, cached in memory by the snapshot's key and the formats
// (docs/design/graphics.md, "Pipeline creation and cache (item 31, as built)").
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <unordered_map>

#include <vulkan/vulkan.h>

#include "common/code_hash.hpp"
#include "gpu/gcn/translate.hpp"
#include "gpu/vk/pipeline_state.hpp"
#include "gpu/vk/rendering.hpp"

namespace bb::vk {

class Device;

// The guest code at a guest address: the readable dwords from there on (to the end of its
// mapping), or none when the address isn't readable.
using CodeReader = std::function<std::span<const std::uint32_t>(std::uint64_t address)>;

// The dynamic state of every pipeline of the cache, which a draw sets before it draws: Vulkan
// 1.3's core dynamic state for what the draw snapshot leaves out of the pipeline key. The
// snapshot (DynamicState) has the viewport, cull mode, front face and blend constants; the draw
// must still set the others (depth and stencil, scissor, depth bias and primitive restart: not
// done).
inline constexpr VkDynamicState graphics_dynamic_states[] = {
    VK_DYNAMIC_STATE_VIEWPORT,
    VK_DYNAMIC_STATE_SCISSOR,
    VK_DYNAMIC_STATE_CULL_MODE,
    VK_DYNAMIC_STATE_FRONT_FACE,
    VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
    VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
    VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
    VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
    VK_DYNAMIC_STATE_STENCIL_OP,
    VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
    VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
    VK_DYNAMIC_STATE_STENCIL_REFERENCE,
    VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
    VK_DYNAMIC_STATE_DEPTH_BIAS,
    VK_DYNAMIC_STATE_BLEND_CONSTANTS,
    VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
};

// A created pipeline and what a draw binds with it.
struct GraphicsPipeline {
    VkPipeline pipeline = VK_NULL_HANDLE;
    // One push-constant range of gcn::graphics_push_size bytes for both stages (the VS's draw
    // block address at gcn::vertex_push_offset, the PS's at gcn::pixel_push_offset). No
    // descriptor sets yet: they come with the textures item (28).
    VkPipelineLayout layout = VK_NULL_HANDLE;
    gcn::ShaderInterface vs, ps;  // ps is empty without a pixel stage
};

class PipelineCache {
public:
    // The device must outlive the cache, and the GPU must be done with the pipelines when it is
    // destroyed. `read` gives the stages' code and the fetch subroutine.
    PipelineCache(const Device& device, CodeReader read);
    ~PipelineCache();
    PipelineCache(const PipelineCache&) = delete;
    PipelineCache& operator=(const PipelineCache&) = delete;

    // The pipeline for `key` drawing into `targets` (their formats and sample count). Made on the
    // first request; an equal request returns the same pipeline while the code of its stages and
    // the fetch subroutine is unchanged (hashed on every request, see docs/design/graphics.md,
    // "Code hash of a cached pipeline"); changed code gets a pipeline of its own. The VS's fetch
    // address counts only for a program that calls the fetch subroutine. Nothing, with `why`
    // saying what, when a
    // stage's code isn't readable, doesn't translate or uses images or samplers (not yet); the
    // error is kept and given again for an equal request, except unreadable code (also an
    // unreadable fetch subroutine a VS needs). Throws std::runtime_error when a Vulkan call
    // fails; nothing is kept then, so the next request tries again.
    const GraphicsPipeline* get(const PipelineKey& key, const RenderingInfo& targets,
                                std::string& why);

    // For tests: called in the middle of creating a pipeline (after its layout exists); what it
    // throws is a Vulkan failure.
    void set_create_hook(std::function<void()> hook) { create_hook_ = std::move(hook); }

    std::size_t size() const { return entries_.size(); }  // pipelines and kept errors
    std::size_t translations() const { return translations_; }

private:
    struct Formats {
        std::array<VkFormat, pm4::color_targets> color{};
        std::uint32_t color_count = 0;
        VkFormat depth = VK_FORMAT_UNDEFINED, stencil = VK_FORMAT_UNDEFINED;
        std::uint32_t samples = 1;
        bool operator==(const Formats&) const = default;
    };
    struct Entry {
        PipelineKey key;
        Formats formats;
        GraphicsPipeline pipeline;
        std::string error;  // why there is no pipeline
        // Of the code it was made from: the stages' programs and the fetch subroutine (empty
        // without a pixel stage or a call).
        common::CodeStamp vs_code, ps_code, fetch_code;
    };
    struct Fetchless {
        gcn::ShaderKey key;
        common::CodeStamp code;  // of the VS program, which must still be that one
    };

    bool fetchless(const StageState& vs, common::CodeProbe& code) const;
    void create(Entry& entry, const gcn::Translation& vs, const gcn::Translation* ps);

    const Device& device_;
    CodeReader read_;
    std::function<void()> create_hook_;
    // By the stages' code addresses (hash_stages), so the nodes, and the pipelines handed out,
    // stay where they are.
    std::unordered_multimap<std::uint64_t, Entry> entries_;
    // The VS programs (code address and key) whose result doesn't depend on the fetch address:
    // they don't call the subroutine, or fail for another reason.
    std::unordered_multimap<std::uint64_t, Fetchless> fetchless_;
    std::size_t translations_ = 0;
};

}  // namespace bb::vk
