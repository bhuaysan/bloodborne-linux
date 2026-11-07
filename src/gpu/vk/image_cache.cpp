#include "gpu/vk/image_cache.hpp"

#include <functional>

#include "common/log.hpp"
#include "gpu/vk/device.hpp"
#include "gpu/vk/formats.hpp"

namespace bb::vk {

namespace {

VkImageAspectFlags aspects_of(VkFormat format) {
    switch (format) {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_D32_SFLOAT:
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    default:
        return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

}  // namespace

std::optional<ImageKey> color_image_key(const pm4::ColorTarget& t) {
    const VkFormat format = color_format(t.format, t.number_type, t.comp_swap);
    if (format == VK_FORMAT_UNDEFINED) {
        return std::nullopt;
    }
    return ImageKey{t.address, format, t.extent, t.slices, t.samples, t.tile_mode};
}

std::optional<ImageKey> depth_image_key(const pm4::DepthTarget& t) {
    const VkFormat format = depth_format(t.z_format, t.stencil);
    if (format == VK_FORMAT_UNDEFINED) {
        return std::nullopt;
    }
    const std::uint64_t address = t.z_format != 0 ? t.z_address : t.stencil_address;
    return ImageKey{address, format, t.extent, t.slices, t.samples, t.tile_mode};
}

std::size_t ImageCache::Hash::operator()(const ImageKey& k) const {
    std::size_t h = std::hash<std::uint64_t>{}(k.address);
    for (const std::uint32_t v : {static_cast<std::uint32_t>(k.format), k.extent.width,
                                  k.extent.height, k.slices, k.samples, k.tile_mode}) {
        h = h * 31 + v;
    }
    return h;
}

ImageCache::~ImageCache() {
    for (const auto& [key, image] : images_) {
        if (image) {
            vkDestroyImage(device_.handle(), image->handle, nullptr);
            vkFreeMemory(device_.handle(), image->memory, nullptr);
        }
    }
}

Image* ImageCache::get(const ImageKey& key) {
    auto it = images_.find(key);
    if (it == images_.end()) {
        it = images_.emplace(key, create(key)).first;
        if (!it->second) {
            common::log(common::LogLevel::warn, "gpu.vk",
                        "no target image at {:#x}: format {}, {}x{}, {} slices, {} samples",
                        key.address, +key.format, key.extent.width, key.extent.height, key.slices,
                        key.samples);
        }
    }
    return it->second.get();
}

std::unique_ptr<Image> ImageCache::create(const ImageKey& key) const {
    const bool depth = aspects_of(key.format) != VK_IMAGE_ASPECT_COLOR_BIT;
    const VkImageUsageFlags usage =
        (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const auto supports = [&](VkFormat format) {
        VkImageFormatProperties p{};
        return vkGetPhysicalDeviceImageFormatProperties(device_.physical(), format,
                                                        VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                                        usage, 0, &p) == VK_SUCCESS &&
               key.extent.width <= p.maxExtent.width && key.extent.height <= p.maxExtent.height &&
               key.slices <= p.maxArrayLayers && (p.sampleCounts & key.samples) != 0;
    };
    VkFormat format = key.format;
    if (format == VK_FORMAT_D16_UNORM_S8_UINT && !supports(format)) {
        format = VK_FORMAT_D32_SFLOAT_S8_UINT;
    }
    if (key.extent.width == 0 || key.extent.height == 0 || key.slices == 0 || !supports(format)) {
        return nullptr;
    }

    auto image = std::make_unique<Image>();
    image->key = key;
    image->format = format;
    image->aspects = aspects_of(format);
    auto info = make<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {key.extent.width, key.extent.height, 1};
    info.mipLevels = 1;
    info.arrayLayers = key.slices;
    info.samples = static_cast<VkSampleCountFlagBits>(key.samples);
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    const VkDevice device = device_.handle();
    check(vkCreateImage(device, &info, nullptr, &image->handle), "vkCreateImage");
    VkMemoryRequirements needs;
    vkGetImageMemoryRequirements(device, image->handle, &needs);
    const auto type =
        device_.memory_type(needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    auto alloc = make<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
    alloc.allocationSize = needs.size;
    alloc.memoryTypeIndex = type.value_or(0);
    VkResult result = type ? vkAllocateMemory(device, &alloc, nullptr, &image->memory)
                           : VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (result == VK_SUCCESS) {
        result = vkBindImageMemory(device, image->handle, image->memory, 0);
    }
    if (result != VK_SUCCESS) {
        vkDestroyImage(device, image->handle, nullptr);
        vkFreeMemory(device, image->memory, nullptr);
        check(result, "vkAllocateMemory/vkBindImageMemory");
    }
    return image;
}

}  // namespace bb::vk
