#include "gpu/vk/compute_pipeline.hpp"

#include <format>
#include <utility>

#include "common/log.hpp"
#include "gpu/gcn/gcn.hpp"
#include "gpu/vk/device.hpp"

namespace bb::vk {

ComputePipelineCache::ComputePipelineCache(const Device& device, CodeReader read)
    : device_(device), read_(std::move(read)) {}

ComputePipelineCache::~ComputePipelineCache() {
    for (auto& [hash, entry] : entries_) destroy(entry);
}

void ComputePipelineCache::destroy(Entry& entry) {
    vkDestroyPipeline(device_.handle(), entry.pipeline.pipeline, nullptr);
    vkDestroyPipelineLayout(device_.handle(), entry.pipeline.layout, nullptr);
    entry.pipeline.pipeline = VK_NULL_HANDLE;
    entry.pipeline.layout = VK_NULL_HANDLE;
}

const ComputePipeline* ComputePipelineCache::get(const ComputeState& state, std::string& why) {
    // The code is read again on every request: an entry counts only while the code at the
    // address is what it was made from. An older entry stays (the GPU may still use its pipeline)
    // and matches again if the code returns.
    const auto code = read_(state.code_address);
    const auto [first, last] = entries_.equal_range(state.code_address);
    for (auto it = first; it != last; ++it) {
        if (it->second.state == state && it->second.stamp.matches(code)) {
            why = it->second.error;
            return it->second.pipeline.pipeline != VK_NULL_HANDLE ? &it->second.pipeline : nullptr;
        }
    }

    // Unreadable code isn't kept: the program may be mapped by the next request.
    if (code.empty()) {
        why = std::format("no readable code at {:#x}", state.code_address);
        return nullptr;
    }
    Entry entry{state, {}, {}, common::CodeStamp::of(code, common::error_stamp_dwords)};
    std::optional<gcn::Translation> translation;
    try {
        ++translations_;
        translation = gcn::translate(code, {}, state.key);
        entry.stamp = common::CodeStamp::of(code, translation->code_dwords);
        if (!translation->interface.images.empty()) {
            entry.error = "images and samplers are not supported yet";
        }
    } catch (const gcn::GcnError& e) {
        entry.error = std::format("the CS at {:#x} doesn't translate: {}", state.code_address,
                                  e.what());
    }
    why = entry.error;
    const auto node = entries_.emplace(state.code_address, std::move(entry));
    Entry& stored = node->second;
    if (!why.empty()) {
        return nullptr;
    }
    // A failing Vulkan call may be transient (out of memory): drop the entry and what it
    // created, so the next request tries again.
    try {
        create(stored, *translation);
    } catch (...) {
        destroy(stored);
        entries_.erase(node);
        throw;
    }
    return &stored.pipeline;
}

void ComputePipelineCache::create(Entry& entry, const gcn::Translation& translation) {
    const VkDevice device = device_.handle();
    ComputePipeline& p = entry.pipeline;
    p.interface = translation.interface;
    const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, compute_push_size};
    auto layout = make<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &range;
    check(vkCreatePipelineLayout(device, &layout, nullptr, &p.layout), "vkCreatePipelineLayout");
    if (create_hook_) create_hook_();

    auto module = make<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
    module.codeSize = translation.spirv.size() * sizeof(std::uint32_t);
    module.pCode = translation.spirv.data();
    VkShaderModule handle = VK_NULL_HANDLE;
    check(vkCreateShaderModule(device, &module, nullptr, &handle), "vkCreateShaderModule");
    auto info = make<VkComputePipelineCreateInfo>(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
    info.stage = make<VkPipelineShaderStageCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = handle;
    info.stage.pName = "main";
    info.layout = p.layout;
    const VkResult result =
        vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &p.pipeline);
    vkDestroyShaderModule(device, handle, nullptr);
    check(result, "vkCreateComputePipelines");
}

}  // namespace bb::vk
