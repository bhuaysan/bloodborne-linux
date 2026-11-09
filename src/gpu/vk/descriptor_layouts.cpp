#include "gpu/vk/descriptor_layouts.hpp"

#include <vector>

#include "gpu/vk/device.hpp"

namespace bb::vk {

DescriptorLayouts::~DescriptorLayouts() {
    for (const auto& [key, layout] : layouts_) {
        vkDestroyDescriptorSetLayout(device_.handle(), layout, nullptr);
    }
}

VkDescriptorSetLayout DescriptorLayouts::get(const gcn::ShaderInterface& interface) {
    const std::pair key{static_cast<std::uint32_t>(interface.images.size()),
                        static_cast<std::uint32_t>(interface.samplers.size())};
    const auto it = layouts_.find(key);
    if (it != layouts_.end()) {
        return it->second;
    }
    // Every set is visible to every stage that uses sets (compute, VS, PS), so a layout fits any.
    constexpr VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT |
                                          VK_SHADER_STAGE_FRAGMENT_BIT |
                                          VK_SHADER_STAGE_COMPUTE_BIT;
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for (std::uint32_t i = 0; i < key.first + key.second; ++i) {
        bindings.push_back({i,
                            i < key.first ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                          : VK_DESCRIPTOR_TYPE_SAMPLER,
                            1, stages, nullptr});
    }
    auto info = make<VkDescriptorSetLayoutCreateInfo>(
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
    info.bindingCount = static_cast<std::uint32_t>(bindings.size());
    info.pBindings = bindings.data();
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    check(vkCreateDescriptorSetLayout(device_.handle(), &info, nullptr, &layout),
          "vkCreateDescriptorSetLayout");
    layouts_.emplace(key, layout);
    return layout;
}

}  // namespace bb::vk
