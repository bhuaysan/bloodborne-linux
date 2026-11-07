// The images of colour and depth targets, cached by guest address and descriptor
// (docs/design/graphics.md, "Render targets and depth").
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>

#include <vulkan/vulkan.h>

#include "gpu/pm4/targets.hpp"

namespace bb::vk {

class Device;

// What a target's image is cached by: its address and descriptor.
struct ImageKey {
    std::uint64_t address = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;  // as color_format or depth_format give it
    pm4::Extent extent;                     // the target's extent
    std::uint32_t slices = 1;  // the layers up to the end of the view (ColorTarget::slices)
    std::uint32_t samples = 1;
    std::uint32_t tile_mode = 0;
    bool operator==(const ImageKey&) const = default;
};

// The key of a colour target's image; nothing when its format has no Vulkan format.
std::optional<ImageKey> color_image_key(const pm4::ColorTarget& target);
// The key of a depth target's image, at the depth address or, for stencil alone, at the stencil
// address; nothing when its depth format has no Vulkan format.
std::optional<ImageKey> depth_image_key(const pm4::DepthTarget& target);

struct Image {
    ImageKey key;
    VkFormat format = VK_FORMAT_UNDEFINED;  // key.format, or its replacement on this device
    VkImageAspectFlags aspects = 0;
    VkImage handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    // The layout the last recorded use left the image in; whoever records a use updates it.
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

class ImageCache {
public:
    // The device must outlive the cache, and the GPU must be done with its images when the
    // cache is destroyed.
    explicit ImageCache(const Device& device) : device_(device) {}
    ~ImageCache();
    ImageCache(const ImageCache&) = delete;
    ImageCache& operator=(const ImageCache&) = delete;

    // The image for `key`, a 2D image with key.slices layers in device-local memory, made on
    // first use with undefined contents. D16_UNORM_S8_UINT, which Vulkan doesn't require,
    // becomes D32_SFLOAT_S8_UINT when the device lacks it. Nothing when the device can't make
    // the image (format, extent, layers or samples), logged once per key.
    Image* get(const ImageKey& key);

private:
    struct Hash {
        std::size_t operator()(const ImageKey& key) const;
    };
    std::unique_ptr<Image> create(const ImageKey& key) const;

    const Device& device_;
    std::unordered_map<ImageKey, std::unique_ptr<Image>, Hash> images_;  // null: unsupported
};

}  // namespace bb::vk
