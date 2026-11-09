#include "gpu/vk/texture_cache.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <functional>

#include "common/log.hpp"
#include "gpu/vk/device.hpp"
#include "gpu/vk/scheduler.hpp"

namespace bb::vk {

namespace {

// The levels of the image: the T#'s, but no more than the extent has.
std::uint32_t level_count(const TextureDesc& d) {
    return std::min<std::uint32_t>(d.last_level + 1, std::bit_width(std::max(d.width, d.height)));
}

std::uint32_t layer_count(const TextureDesc& d) {
    return d.type == image_type_2d_array ? d.depth : 1;
}

// A 64-bit hash of the bytes (multiply and fold per 8 bytes). Not cryptographic: it only tells
// changed texels from unchanged ones.
std::uint64_t hash_bytes(std::span<const std::byte> bytes) {
    constexpr std::uint64_t k = 0x9e3779b97f4a7c15ull;
    std::uint64_t h = bytes.size() * k;
    std::size_t i = 0;
    for (; i + 8 <= bytes.size(); i += 8) {
        std::uint64_t w;
        std::memcpy(&w, bytes.data() + i, 8);
        h = (h ^ w) * k;
        h ^= h >> 29;
    }
    std::uint64_t tail = 0;
    std::memcpy(&tail, bytes.data() + i, bytes.size() - i);
    h = (h ^ tail) * k;
    return h ^ (h >> 32);
}

VkComponentSwizzle swizzle(std::uint32_t sel) {
    switch (sel) {
    case 0: return VK_COMPONENT_SWIZZLE_ZERO;
    case 1: return VK_COMPONENT_SWIZZLE_ONE;
    case 4: return VK_COMPONENT_SWIZZLE_R;
    case 5: return VK_COMPONENT_SWIZZLE_G;
    case 6: return VK_COMPONENT_SWIZZLE_B;
    case 7: return VK_COMPONENT_SWIZZLE_A;
    default: return VK_COMPONENT_SWIZZLE_ZERO;  // reserved selects read 0
    }
}

// ISA Table 8.12. The mirror-once and half-border modes have no Vulkan mode on a device
// without samplerMirrorClampToEdge (not enabled): they take the nearest of clamp and mirror.
VkSamplerAddressMode address_mode(std::uint32_t clamp) {
    switch (clamp) {
    case 0: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 1: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 2: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case 3: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;  // 4 and 6; 5 and 7 mirror once
    }
}

VkCompareOp compare_op(std::uint32_t f) {
    constexpr VkCompareOp ops[] = {VK_COMPARE_OP_NEVER,     VK_COMPARE_OP_LESS,
                                   VK_COMPARE_OP_EQUAL,     VK_COMPARE_OP_LESS_OR_EQUAL,
                                   VK_COMPARE_OP_GREATER,   VK_COMPARE_OP_NOT_EQUAL,
                                   VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_ALWAYS};
    return ops[f & 7];
}

struct Staging {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    std::byte* bytes = nullptr;
};

Staging make_staging(const Device& device, VkDeviceSize size) {
    Staging s;
    const VkDevice d = device.handle();
    auto info = make<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
    info.size = size;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    check(vkCreateBuffer(d, &info, nullptr, &s.buffer), "vkCreateBuffer");
    VkMemoryRequirements needs;
    vkGetBufferMemoryRequirements(d, s.buffer, &needs);
    const auto type = device.memory_type(needs.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    auto alloc = make<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
    alloc.allocationSize = needs.size;
    alloc.memoryTypeIndex = type.value_or(0);
    VkResult result = type ? vkAllocateMemory(d, &alloc, nullptr, &s.memory)
                           : VK_ERROR_OUT_OF_HOST_MEMORY;
    void* map = nullptr;
    if (result == VK_SUCCESS) {
        result = vkBindBufferMemory(d, s.buffer, s.memory, 0);
    }
    if (result == VK_SUCCESS) {
        result = vkMapMemory(d, s.memory, 0, VK_WHOLE_SIZE, 0, &map);
    }
    if (result != VK_SUCCESS) {
        vkDestroyBuffer(d, s.buffer, nullptr);
        vkFreeMemory(d, s.memory, nullptr);
        check(result, "texture staging buffer");
    }
    s.bytes = static_cast<std::byte*>(map);
    return s;
}

}  // namespace

std::size_t TextureCache::ImageKeyHash::operator()(const ImageKey& k) const {
    std::size_t h = std::hash<std::uint64_t>{}(k.address);
    for (const std::uint32_t v : {k.data_format, k.num_format, k.width, k.height, k.pitch, k.type,
                                  k.layers, k.levels, k.tile_mode}) {
        h = h * 31 + v;
    }
    return h;
}

std::size_t TextureCache::SamplerKeyHash::operator()(const SamplerKey& k) const {
    const SamplerDesc& s = k.desc;
    std::size_t h = k.compare * 4 + k.limits.linear * 2 + k.limits.integer;
    for (const std::uint32_t v :
         {s.clamp_x, s.clamp_y, s.clamp_z, s.max_aniso, s.depth_compare, s.mag_filter,
          s.min_filter, s.mip_filter, s.border_color, std::bit_cast<std::uint32_t>(s.min_lod),
          std::bit_cast<std::uint32_t>(s.max_lod), std::bit_cast<std::uint32_t>(s.lod_bias)}) {
        h = h * 31 + v;
    }
    return h;
}

TextureCache::TextureCache(Device& device, Scheduler& scheduler)
    : device_(device), scheduler_(scheduler) {}

TextureCache::~TextureCache() {
    scheduler_.finish();
    for (auto& [key, image] : images_) {
        if (image) {
            destroy(*image);
        }
    }
    for (const auto& [key, sampler] : samplers_) {
        vkDestroySampler(device_.handle(), sampler, nullptr);
    }
}

void TextureCache::destroy(Image& image) {
    for (const auto& [key, view] : image.views) {
        vkDestroyImageView(device_.handle(), view->view, nullptr);
    }
    vkDestroyImage(device_.handle(), image.handle, nullptr);
    vkFreeMemory(device_.handle(), image.memory, nullptr);
}

std::optional<tile::Layout> TextureCache::guest_layout(const TextureDesc& d) {
    if (d.type != image_type_2d && d.type != image_type_2d_array) {
        return std::nullopt;
    }
    const auto format = texture_format(d.data_format, d.num_format);
    if (!format || d.pitch < d.width) {
        return std::nullopt;
    }
    return tile::layout({.tile_mode = d.tile_mode,
                         .bits = format->bits,
                         .pitch = d.pitch,
                         .height = d.height,
                         .layers = layer_count(d),
                         .levels = level_count(d),
                         .blocks = format->blocks});
}

std::unique_ptr<TextureCache::Image> TextureCache::create(const ImageKey& key,
                                                          const TextureFormat& format,
                                                          const tile::Layout& layout) const {
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImageFormatProperties p{};
    if (vkGetPhysicalDeviceImageFormatProperties(device_.physical(), format.format,
                                                 VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage,
                                                 0, &p) != VK_SUCCESS ||
        key.width > p.maxExtent.width || key.height > p.maxExtent.height ||
        key.layers > p.maxArrayLayers || key.levels > p.maxMipLevels) {
        return nullptr;
    }
    // Vulkan wants a buffer offset to be a multiple of 4 and of the element size.
    for (const tile::Level& level : layout.levels) {
        if (level.offset % std::max(4u, layout.bytes) != 0 ||
            level.layer_size % std::max(4u, layout.bytes) != 0) {
            return nullptr;
        }
    }
    auto image = std::make_unique<Image>();
    image->format = format.format;
    auto info = make<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format.format;
    info.extent = {key.width, key.height, 1};
    info.mipLevels = key.levels;
    info.arrayLayers = key.layers;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
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

void TextureCache::upload(Image& image, const ImageKey& key, const tile::Layout& layout,
                          std::span<const std::byte> guest) {
    const Staging staging = make_staging(device_, layout.size);
    tile::detile(layout, guest.first(layout.size), {staging.bytes, layout.size});
    const bool blocks = texture_format(key.data_format, key.num_format)->blocks;
    const std::uint32_t texels = blocks ? 4 : 1;  // texels per element along a row
    std::vector<VkBufferImageCopy> regions;
    for (std::uint32_t i = 0; i < layout.levels.size(); ++i) {
        const tile::Level& level = layout.levels[i];
        for (std::uint32_t layer = 0; layer < key.layers; ++layer) {
            VkBufferImageCopy r{};
            r.bufferOffset = level.offset + std::uint64_t{layer} * level.layer_size;
            r.bufferRowLength = level.pitch * texels;
            r.bufferImageHeight = level.height * texels;
            r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, layer, 1};
            r.imageExtent = {std::max(key.width >> i, 1u), std::max(key.height >> i, 1u), 1};
            regions.push_back(r);
        }
    }

    const VkCommandBuffer commands = scheduler_.commands();
    const auto barrier = [&](VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 src_stage,
                             VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                             VkAccessFlags2 dst_access) {
        auto b = make<VkImageMemoryBarrier2>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2);
        b.srcStageMask = src_stage;
        b.srcAccessMask = src_access;
        b.dstStageMask = dst_stage;
        b.dstAccessMask = dst_access;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image.handle;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0,
                              VK_REMAINING_ARRAY_LAYERS};
        auto dependency = make<VkDependencyInfo>(VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(commands, &dependency);
    };
    // The old contents are replaced whole, so the layout change drops them. Earlier reads of the
    // image (a draw of this submission) finish before the copy writes.
    barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdCopyBufferToImage(commands, staging.buffer, image.handle,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<std::uint32_t>(regions.size()), regions.data());
    barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, TextureView::layout,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
    image.layout = TextureView::layout;
    ++uploads_;
    scheduler_.defer([&device = device_, staging] {
        vkDestroyBuffer(device.handle(), staging.buffer, nullptr);
        vkFreeMemory(device.handle(), staging.memory, nullptr);
    });
}

const TextureView* TextureCache::get(const TextureDesc& d, std::span<const std::byte> guest) {
    const auto format = texture_format(d.data_format, d.num_format);
    const auto layout = guest_layout(d);
    const ImageKey key{d.address, d.data_format, d.num_format, d.width,     d.height,
                       d.pitch,   d.type,        layer_count(d), level_count(d), d.tile_mode};
    auto it = images_.find(key);
    if (it == images_.end()) {
        std::unique_ptr<Image> image = layout ? create(key, *format, *layout) : nullptr;
        if (!image) {
            common::log(common::LogLevel::warn, "gpu.vk",
                        "no texture at {:#x}: type {}, format {}/{}, {}x{} (pitch {}), tile mode "
                        "{}, {} levels",
                        d.address, d.type, d.data_format, d.num_format, d.width, d.height,
                        d.pitch, d.tile_mode, key.levels);
        }
        it = images_.emplace(key, std::move(image)).first;
    }
    Image* image = it->second.get();
    if (image == nullptr) {
        return nullptr;
    }
    if (guest.size() < layout->size) {
        common::log(common::LogLevel::warn, "gpu.vk",
                    "texture at {:#x}: {} guest bytes given, {} needed", d.address, guest.size(),
                    layout->size);
        return nullptr;
    }
    if (image->checked != epoch_) {
        const std::uint64_t hash = hash_bytes(guest.first(layout->size));
        if (image->layout == VK_IMAGE_LAYOUT_UNDEFINED || hash != image->hash) {
            upload(*image, key, *layout, guest);
        }
        image->hash = hash;
        image->checked = epoch_;
    }

    const std::uint32_t base_level = std::min(d.base_level, key.levels - 1);
    const std::uint32_t base_layer = std::min(d.base_array, key.layers - 1);
    const std::uint32_t last_layer = std::clamp(d.last_array, base_layer, key.layers - 1);
    const ViewKey view_key{base_level, key.levels - base_level, base_layer,
                           key.type == image_type_2d_array ? last_layer - base_layer + 1 : 1,
                           d.dst_sel};
    auto& slot = image->views[view_key];
    if (!slot) {
        auto info = make<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
        info.image = image->handle;
        info.viewType = key.type == image_type_2d_array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                                                        : VK_IMAGE_VIEW_TYPE_2D;
        info.format = image->format;
        info.components = {swizzle(d.dst_sel[0]), swizzle(d.dst_sel[1]), swizzle(d.dst_sel[2]),
                           swizzle(d.dst_sel[3])};
        info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, view_key.base_level, view_key.levels,
                                 view_key.base_array, view_key.layers};
        auto view = std::make_unique<TextureView>();
        view->image = image->handle;
        view->format = image->format;
        VkFormatProperties properties;
        vkGetPhysicalDeviceFormatProperties(device_.physical(), image->format, &properties);
        view->filter_linear = (properties.optimalTilingFeatures &
                               VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
        view->integer = d.num_format == 4 || d.num_format == 5;  // UINT, SINT
        check(vkCreateImageView(device_.handle(), &info, nullptr, &view->view),
              "vkCreateImageView");
        slot = std::move(view);
    }
    return slot.get();
}

VkSampler TextureCache::sampler(const SamplerDesc& s, bool compare, const SamplerLimits& limits) {
    const SamplerKey key{s, compare, limits};
    const auto it = samplers_.find(key);
    if (it != samplers_.end()) {
        return it->second;
    }
    VkSampler handle = VK_NULL_HANDLE;
    if (const auto why = unsupported_sampler(s)) {
        common::log(common::LogLevel::error, "gpu.vk", "unsupported sampler: {}", *why);
    } else {
        // Anisotropic filtering is not enabled on the device (samplerAnisotropy): the
        // anisotropic filters (2 and 3) filter as point and bilinear.
        const auto filter = [&](std::uint32_t f) {
            return (f & 1) != 0 && limits.linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        };
        const float bias_limit = device_.properties().core.limits.maxSamplerLodBias;
        auto info = make<VkSamplerCreateInfo>(VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
        info.magFilter = filter(s.mag_filter);
        info.minFilter = filter(s.min_filter);
        info.mipmapMode = s.mip_filter == 2 && limits.linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                             : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.addressModeU = address_mode(s.clamp_x);
        info.addressModeV = address_mode(s.clamp_y);
        info.addressModeW = address_mode(s.clamp_z);
        info.mipLodBias = std::clamp(s.lod_bias, -bias_limit, bias_limit);
        info.compareEnable = compare;
        info.compareOp = compare_op(s.depth_compare);
        // No mip filter: the base level only.
        info.minLod = s.mip_filter == 0 ? 0.0f : s.min_lod;
        info.maxLod = s.mip_filter == 0 ? 0.0f : std::max(s.max_lod, s.min_lod);
        constexpr VkBorderColor borders[] = {
            VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK, VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
            VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE, VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK};
        constexpr VkBorderColor int_borders[] = {
            VK_BORDER_COLOR_INT_TRANSPARENT_BLACK, VK_BORDER_COLOR_INT_OPAQUE_BLACK,
            VK_BORDER_COLOR_INT_OPAQUE_WHITE, VK_BORDER_COLOR_INT_TRANSPARENT_BLACK};
        // 3: a border colour register. A float border colour on an integer image is invalid.
        info.borderColor = (limits.integer ? int_borders : borders)[s.border_color & 3];
        check(vkCreateSampler(device_.handle(), &info, nullptr, &handle), "vkCreateSampler");
    }
    samplers_.emplace(key, handle);
    return handle;
}

}  // namespace bb::vk
