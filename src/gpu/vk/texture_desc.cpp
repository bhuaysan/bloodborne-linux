#include "gpu/vk/texture_desc.hpp"

namespace bb::vk {

namespace {

constexpr std::uint32_t field(std::uint32_t word, unsigned lo, unsigned bits) {
    return (word >> lo) & ((1u << bits) - 1);
}

// GCN number formats (ISA Table 8.11, as far as textures use them).
enum Num : std::uint32_t { unorm = 0, snorm = 1, uint_ = 4, sint = 5, sfloat = 7, srgb = 9 };

}  // namespace

std::optional<TextureDesc> decode_texture(std::span<const std::uint32_t, 8> d) {
    const std::uint32_t type = field(d[3], 28, 4);
    if ((type & 8) == 0) {
        return std::nullopt;
    }
    TextureDesc t;
    t.address = (std::uint64_t{field(d[1], 0, 6)} << 32 | d[0]) << 8;
    t.data_format = field(d[1], 20, 6);
    t.num_format = field(d[1], 26, 4);
    t.width = field(d[2], 0, 14) + 1;
    t.height = field(d[2], 14, 14) + 1;
    for (unsigned i = 0; i < 4; ++i) {
        t.dst_sel[i] = field(d[3], 3 * i, 3);
    }
    t.base_level = field(d[3], 12, 4);
    t.last_level = field(d[3], 16, 4);
    t.tile_mode = field(d[3], 20, 5);
    t.type = type;
    t.depth = field(d[4], 0, 13) + 1;
    t.pitch = field(d[4], 13, 14) + 1;
    t.base_array = field(d[5], 0, 13);
    t.last_array = field(d[5], 13, 13);
    return t;
}

std::optional<TextureFormat> texture_format(std::uint32_t dfmt, std::uint32_t nfmt) {
    struct Row {
        std::uint32_t dfmt, nfmt;
        VkFormat format;
        std::uint32_t bits;
        bool blocks;
    };
    static constexpr Row rows[] = {
        {1, unorm, VK_FORMAT_R8_UNORM, 8, false},
        {1, snorm, VK_FORMAT_R8_SNORM, 8, false},
        {1, uint_, VK_FORMAT_R8_UINT, 8, false},
        {1, sint, VK_FORMAT_R8_SINT, 8, false},
        {1, srgb, VK_FORMAT_R8_SRGB, 8, false},
        {2, unorm, VK_FORMAT_R16_UNORM, 16, false},
        {2, snorm, VK_FORMAT_R16_SNORM, 16, false},
        {2, uint_, VK_FORMAT_R16_UINT, 16, false},
        {2, sint, VK_FORMAT_R16_SINT, 16, false},
        {2, sfloat, VK_FORMAT_R16_SFLOAT, 16, false},
        {3, unorm, VK_FORMAT_R8G8_UNORM, 16, false},
        {3, snorm, VK_FORMAT_R8G8_SNORM, 16, false},
        {3, uint_, VK_FORMAT_R8G8_UINT, 16, false},
        {3, sint, VK_FORMAT_R8G8_SINT, 16, false},
        {3, srgb, VK_FORMAT_R8G8_SRGB, 16, false},
        {4, uint_, VK_FORMAT_R32_UINT, 32, false},
        {4, sint, VK_FORMAT_R32_SINT, 32, false},
        {4, sfloat, VK_FORMAT_R32_SFLOAT, 32, false},
        {5, unorm, VK_FORMAT_R16G16_UNORM, 32, false},
        {5, snorm, VK_FORMAT_R16G16_SNORM, 32, false},
        {5, uint_, VK_FORMAT_R16G16_UINT, 32, false},
        {5, sint, VK_FORMAT_R16G16_SINT, 32, false},
        {5, sfloat, VK_FORMAT_R16G16_SFLOAT, 32, false},
        {7, sfloat, VK_FORMAT_B10G11R11_UFLOAT_PACK32, 32, false},  // 11_11_10
        {9, unorm, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 32, false},  // 2_10_10_10
        {9, snorm, VK_FORMAT_A2B10G10R10_SNORM_PACK32, 32, false},
        {9, uint_, VK_FORMAT_A2B10G10R10_UINT_PACK32, 32, false},
        {9, sint, VK_FORMAT_A2B10G10R10_SINT_PACK32, 32, false},
        {10, unorm, VK_FORMAT_R8G8B8A8_UNORM, 32, false},
        {10, snorm, VK_FORMAT_R8G8B8A8_SNORM, 32, false},
        {10, uint_, VK_FORMAT_R8G8B8A8_UINT, 32, false},
        {10, sint, VK_FORMAT_R8G8B8A8_SINT, 32, false},
        {10, srgb, VK_FORMAT_R8G8B8A8_SRGB, 32, false},
        {11, uint_, VK_FORMAT_R32G32_UINT, 64, false},
        {11, sint, VK_FORMAT_R32G32_SINT, 64, false},
        {11, sfloat, VK_FORMAT_R32G32_SFLOAT, 64, false},
        {12, unorm, VK_FORMAT_R16G16B16A16_UNORM, 64, false},
        {12, snorm, VK_FORMAT_R16G16B16A16_SNORM, 64, false},
        {12, uint_, VK_FORMAT_R16G16B16A16_UINT, 64, false},
        {12, sint, VK_FORMAT_R16G16B16A16_SINT, 64, false},
        {12, sfloat, VK_FORMAT_R16G16B16A16_SFLOAT, 64, false},
        {14, uint_, VK_FORMAT_R32G32B32A32_UINT, 128, false},
        {14, sint, VK_FORMAT_R32G32B32A32_SINT, 128, false},
        {14, sfloat, VK_FORMAT_R32G32B32A32_SFLOAT, 128, false},
        {35, unorm, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 64, true},
        {35, srgb, VK_FORMAT_BC1_RGBA_SRGB_BLOCK, 64, true},
        {36, unorm, VK_FORMAT_BC2_UNORM_BLOCK, 128, true},
        {36, srgb, VK_FORMAT_BC2_SRGB_BLOCK, 128, true},
        {37, unorm, VK_FORMAT_BC3_UNORM_BLOCK, 128, true},
        {37, srgb, VK_FORMAT_BC3_SRGB_BLOCK, 128, true},
        {38, unorm, VK_FORMAT_BC4_UNORM_BLOCK, 64, true},
        {38, snorm, VK_FORMAT_BC4_SNORM_BLOCK, 64, true},
        {39, unorm, VK_FORMAT_BC5_UNORM_BLOCK, 128, true},
        {39, snorm, VK_FORMAT_BC5_SNORM_BLOCK, 128, true},
        {40, unorm, VK_FORMAT_BC6H_UFLOAT_BLOCK, 128, true},
        {40, snorm, VK_FORMAT_BC6H_SFLOAT_BLOCK, 128, true},
        {41, unorm, VK_FORMAT_BC7_UNORM_BLOCK, 128, true},
        {41, srgb, VK_FORMAT_BC7_SRGB_BLOCK, 128, true},
    };
    for (const Row& row : rows) {
        if (row.dfmt == dfmt && row.nfmt == nfmt) {
            return TextureFormat{row.format, row.bits, row.blocks};
        }
    }
    return std::nullopt;
}

SamplerDesc decode_sampler(std::span<const std::uint32_t, 4> d) {
    SamplerDesc s;
    s.clamp_x = field(d[0], 0, 3);
    s.clamp_y = field(d[0], 3, 3);
    s.clamp_z = field(d[0], 6, 3);
    s.max_aniso = field(d[0], 9, 3);
    s.depth_compare = field(d[0], 12, 3);
    s.force_unnormalized = field(d[0], 15, 1) != 0;
    s.min_lod = static_cast<float>(field(d[1], 0, 12)) / 256.0f;
    s.max_lod = static_cast<float>(field(d[1], 12, 12)) / 256.0f;
    // Signed, 6.8 fixed point.
    const auto bias = static_cast<std::int32_t>((field(d[2], 0, 14) ^ 0x2000u)) - 0x2000;
    s.lod_bias = static_cast<float>(bias) / 256.0f;
    s.mag_filter = field(d[2], 20, 2);
    s.min_filter = field(d[2], 22, 2);
    s.mip_filter = field(d[2], 26, 2);
    s.border_color = field(d[3], 30, 2);
    return s;
}

std::optional<std::string> unsupported_sampler(const SamplerDesc& s) {
    if (s.force_unnormalized) {
        return "the S# sets FORCE_UNNORMALIZED (S# dword 0 bit 15), which a VkSampler can't do "
               "for implicit LOD, array and cube images";
    }
    return std::nullopt;
}

}  // namespace bb::vk
