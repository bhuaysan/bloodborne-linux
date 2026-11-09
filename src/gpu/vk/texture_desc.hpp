// T# and S# decoded (docs/design/graphics.md, "Textures and samplers (item 28)"): the image
// descriptor's fields, the Vulkan format of its data and number format, and the sampler state.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include <vulkan/vulkan.h>

namespace bb::vk {

// T# type field values this backend knows (ISA 8.2.5).
inline constexpr std::uint32_t image_type_2d = 9;
inline constexpr std::uint32_t image_type_2d_array = 13;

// The fields of a T# (8 dwords) that make or view an image.
struct TextureDesc {
    std::uint64_t address = 0;  // of the base level, bytes
    std::uint32_t data_format = 0, num_format = 0;
    std::uint32_t width = 1, height = 1;  // of level 0, pixels
    std::uint32_t pitch = 1;              // of level 0, pixels
    std::uint32_t type = 0;
    std::uint32_t depth = 1;  // the T#'s depth field + 1: the layers of an array
    std::uint32_t base_level = 0, last_level = 0;
    std::uint32_t base_array = 0, last_array = 0;
    std::uint32_t tile_mode = 0;
    std::array<std::uint32_t, 4> dst_sel{4, 5, 6, 7};  // x, y, z, w: 0 zero, 1 one, 4 to 7 R to A
    bool operator==(const TextureDesc&) const = default;
};

// Nothing when the dwords are not an image descriptor (a V#'s type field is 0 to 7).
std::optional<TextureDesc> decode_texture(std::span<const std::uint32_t, 8> dwords);

struct TextureFormat {
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::uint32_t bits = 0;  // per element, as bb::tile::Surface takes it
    bool blocks = false;     // a BC format
};
// The format of a T# with DATA_FORMAT and NUM_FORMAT, with the memory layout of the guest's
// element. Nothing for the combinations without a Vulkan format of that layout.
std::optional<TextureFormat> texture_format(std::uint32_t data_format, std::uint32_t num_format);

// The fields of an S# (4 dwords).
struct SamplerDesc {
    std::uint32_t clamp_x = 0, clamp_y = 0, clamp_z = 0;  // 0 wrap ... 7 (ISA Table 8.12)
    std::uint32_t max_aniso = 0;                          // 0 to 4: ratio 1, 2, 4, 8, 16
    std::uint32_t depth_compare = 0;                      // 0 never ... 7 always
    bool force_unnormalized = false;
    std::uint32_t mag_filter = 0, min_filter = 0;  // 0 point, 1 bilinear, 2 and 3 anisotropic
    std::uint32_t mip_filter = 0;                  // 0 none, 1 point, 2 linear
    float min_lod = 0, max_lod = 0, lod_bias = 0;
    std::uint32_t border_color = 0;  // 0 transparent black, 1 opaque black, 2 opaque white, 3 reg
    bool operator==(const SamplerDesc&) const = default;
};
SamplerDesc decode_sampler(std::span<const std::uint32_t, 4> dwords);

// Why a draw can't use the sampler, or nothing: a force-unnormalized S# doesn't fit a
// VkSampler for the forms the translator emits (see "Force unnormalized S#").
std::optional<std::string> unsupported_sampler(const SamplerDesc& sampler);

}  // namespace bb::vk
