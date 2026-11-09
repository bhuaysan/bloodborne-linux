// Textures and samplers from T# and S# (docs/design/graphics.md, "Textures and samplers
// (item 28)"): images cached by address and descriptor, uploaded and detiled from guest memory,
// and samplers cached by their state.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>

#include <vulkan/vulkan.h>

#include "gpu/tile/tiling.hpp"
#include "gpu/vk/texture_desc.hpp"

namespace bb::vk {

class Device;
class Scheduler;

// What get() returns: the view for a T#, on an image that holds the guest's texels.
struct TextureView {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    bool filter_linear = true;  // the format supports a linear filter when sampled
    bool integer = false;       // an integer number format (UINT or SINT)
    // The layout of the image and the view: always SHADER_READ_ONLY_OPTIMAL once get() returns.
    static constexpr VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
};

// What the images that a sampler is used with allow of it (a sampler is paired with an image only
// in the shader).
struct SamplerLimits {
    bool linear = true;     // every image's format supports linear filters
    bool integer = false;   // some image has an integer format: integer border colours
    bool operator==(const SamplerLimits&) const = default;
};

class TextureCache {
public:
    // The device and scheduler must outlive the cache. The destructor waits for the scheduler's
    // work before it destroys anything.
    TextureCache(Device& device, Scheduler& scheduler);
    ~TextureCache();
    TextureCache(const TextureCache&) = delete;
    TextureCache& operator=(const TextureCache&) = delete;

    // The surface layout of a T# in guest memory; layout->size is how many bytes from
    // desc.address get() reads. Nothing for a T# the cache can't make (type other than 2D and 2D
    // array, a format without a Vulkan format, a tile mode the detiler doesn't know, a pitch
    // under the width).
    static std::optional<tile::Layout> guest_layout(const TextureDesc& desc);

    // A new submission begins: textures are checked against guest memory again at their next use.
    void begin_submission() { ++epoch_; }

    // The view for `desc`. `guest` is the guest memory at desc.address, at least
    // guest_layout(desc)->size bytes. The image is made on first use; on first use in a
    // submission its guest bytes are hashed and, when new or changed, detiled on the CPU and
    // copied in by commands recorded into the scheduler's current command buffer, so call it
    // outside a render pass. The view covers the T#'s mip levels from base_level to last_level
    // and layers base_array to last_array, with dst_sel as its component swizzle. Nothing, logged
    // once per descriptor, when the cache can't make it or `guest` is too short.
    const TextureView* get(const TextureDesc& desc, std::span<const std::byte> guest);

    // The sampler for `sampler`. With `compare` the sampler compares with its depth compare
    // function (for the image_sample_c forms). `limits` turns the linear filters into nearest ones
    // and picks integer border colours, as a Vulkan sampler used with such an image needs. A null
    // handle, logged once, when unsupported_sampler(sampler) says so. The cache owns it.
    VkSampler sampler(const SamplerDesc& sampler, bool compare = false,
                      const SamplerLimits& limits = {});

    // Counts, for tests.
    std::size_t images() const { return images_.size(); }
    std::size_t samplers() const { return samplers_.size(); }
    std::size_t uploads() const { return uploads_; }

private:
    // What an image is cached by: the T# without its view fields.
    struct ImageKey {
        std::uint64_t address;
        std::uint32_t data_format, num_format, width, height, pitch, type, layers, levels,
            tile_mode;
        bool operator==(const ImageKey&) const = default;
    };
    struct ImageKeyHash {
        std::size_t operator()(const ImageKey& key) const;
    };
    struct ViewKey {
        std::uint32_t base_level, levels, base_array, layers;
        std::array<std::uint32_t, 4> dst_sel;
        auto operator<=>(const ViewKey&) const = default;
    };
    struct Image {
        VkImage handle = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        std::uint64_t hash = 0;
        std::uint64_t checked = 0;  // the epoch of the last hash
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        std::map<ViewKey, std::unique_ptr<TextureView>> views;
    };
    struct SamplerKey {
        SamplerDesc desc;
        bool compare;
        SamplerLimits limits;
        bool operator==(const SamplerKey&) const = default;
    };
    struct SamplerKeyHash {
        std::size_t operator()(const SamplerKey& key) const;
    };

    std::unique_ptr<Image> create(const ImageKey& key, const TextureFormat& format,
                                  const tile::Layout& layout) const;
    void upload(Image& image, const ImageKey& key, const tile::Layout& layout,
                std::span<const std::byte> guest);
    void destroy(Image& image);

    Device& device_;
    Scheduler& scheduler_;
    std::uint64_t epoch_ = 1;
    std::size_t uploads_ = 0;
    std::unordered_map<ImageKey, std::unique_ptr<Image>, ImageKeyHash> images_;  // null: unsupported
    std::unordered_map<SamplerKey, VkSampler, SamplerKeyHash> samplers_;  // null: unsupported
};

}  // namespace bb::vk
