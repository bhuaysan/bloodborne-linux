// Descriptor set layouts of the translator's image and sampler bindings (docs/design/graphics.md,
// "Image sampling" and "Textures and samplers (item 28)"): image i of a stage's interface is
// binding i (a sampled image), sampler j is binding images.size() + j (a sampler).
#pragma once

#include <cstdint>
#include <map>
#include <utility>

#include <vulkan/vulkan.h>

#include "gpu/gcn/translate.hpp"

namespace bb::vk {

class Device;

// The layouts for stage interfaces, made on first use. Layouts that are defined alike are
// compatible, so each owner (a pipeline cache, the TextureBinder that allocates the sets) can keep
// a cache of its own and a set of one fits a pipeline layout of another.
class DescriptorLayouts {
public:
    explicit DescriptorLayouts(const Device& device) : device_(device) {}
    ~DescriptorLayouts();
    DescriptorLayouts(const DescriptorLayouts&) = delete;
    DescriptorLayouts& operator=(const DescriptorLayouts&) = delete;

    // The layout for the interface's images and samplers; one without bindings when it has none.
    // Throws std::runtime_error when Vulkan fails.
    VkDescriptorSetLayout get(const gcn::ShaderInterface& interface);

private:
    const Device& device_;
    std::map<std::pair<std::uint32_t, std::uint32_t>, VkDescriptorSetLayout> layouts_;
};

}  // namespace bb::vk
