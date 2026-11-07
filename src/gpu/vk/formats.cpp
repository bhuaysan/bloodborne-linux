// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/video_core/renderer_vulkan/liverpool_to_vk.cpp @ff1b7a2e87eef8f301377e0cabbbeeeb73cc3b49
// Changed for bloodborne-linux, 2026-11-07: only the formats a colour target can have (the
// rows whose flags include Mrt), keyed by CB number types, without the remapped 11_11_10 and
// 10_10_10_2; B8G8R8A8 for the alternate swap; the depth table as a function.

#include "gpu/vk/formats.hpp"

namespace bb::vk {

namespace {

// CB_COLOR<n>_INFO.NUMBER_TYPE.
enum Number : std::uint8_t { unorm = 0, snorm = 1, uint = 4, sint = 5, srgb = 6, sfloat = 7 };

struct Row {
    std::uint8_t format;  // CB_COLOR<n>_INFO.FORMAT
    Number number;
    VkFormat vk;
};

constexpr Row rows[] = {
    {1, unorm, VK_FORMAT_R8_UNORM},  // 8
    {1, snorm, VK_FORMAT_R8_SNORM},
    {1, uint, VK_FORMAT_R8_UINT},
    {1, sint, VK_FORMAT_R8_SINT},
    {1, srgb, VK_FORMAT_R8_SRGB},
    {2, unorm, VK_FORMAT_R16_UNORM},  // 16
    {2, snorm, VK_FORMAT_R16_SNORM},
    {2, uint, VK_FORMAT_R16_UINT},
    {2, sint, VK_FORMAT_R16_SINT},
    {2, sfloat, VK_FORMAT_R16_SFLOAT},
    {3, unorm, VK_FORMAT_R8G8_UNORM},  // 8_8
    {3, snorm, VK_FORMAT_R8G8_SNORM},
    {3, uint, VK_FORMAT_R8G8_UINT},
    {3, sint, VK_FORMAT_R8G8_SINT},
    {3, srgb, VK_FORMAT_R8G8_SRGB},
    {4, uint, VK_FORMAT_R32_UINT},  // 32
    {4, sint, VK_FORMAT_R32_SINT},
    {4, sfloat, VK_FORMAT_R32_SFLOAT},
    {5, unorm, VK_FORMAT_R16G16_UNORM},  // 16_16
    {5, snorm, VK_FORMAT_R16G16_SNORM},
    {5, uint, VK_FORMAT_R16G16_UINT},
    {5, sint, VK_FORMAT_R16G16_SINT},
    {5, sfloat, VK_FORMAT_R16G16_SFLOAT},
    {6, sfloat, VK_FORMAT_B10G11R11_UFLOAT_PACK32},  // 10_11_11
    {9, unorm, VK_FORMAT_A2B10G10R10_UNORM_PACK32},  // 2_10_10_10
    {9, snorm, VK_FORMAT_A2B10G10R10_SNORM_PACK32},
    {9, uint, VK_FORMAT_A2B10G10R10_UINT_PACK32},
    {9, sint, VK_FORMAT_A2B10G10R10_SINT_PACK32},
    {10, unorm, VK_FORMAT_R8G8B8A8_UNORM},  // 8_8_8_8
    {10, snorm, VK_FORMAT_R8G8B8A8_SNORM},
    {10, uint, VK_FORMAT_R8G8B8A8_UINT},
    {10, sint, VK_FORMAT_R8G8B8A8_SINT},
    {10, srgb, VK_FORMAT_R8G8B8A8_SRGB},
    {11, uint, VK_FORMAT_R32G32_UINT},  // 32_32
    {11, sint, VK_FORMAT_R32G32_SINT},
    {11, sfloat, VK_FORMAT_R32G32_SFLOAT},
    {12, unorm, VK_FORMAT_R16G16B16A16_UNORM},  // 16_16_16_16
    {12, snorm, VK_FORMAT_R16G16B16A16_SNORM},
    {12, uint, VK_FORMAT_R16G16B16A16_UINT},
    {12, sint, VK_FORMAT_R16G16B16A16_SINT},
    {12, sfloat, VK_FORMAT_R16G16B16A16_SFLOAT},
    {14, uint, VK_FORMAT_R32G32B32A32_UINT},  // 32_32_32_32
    {14, sint, VK_FORMAT_R32G32B32A32_SINT},
    {14, sfloat, VK_FORMAT_R32G32B32A32_SFLOAT},
    {16, unorm, VK_FORMAT_R5G6B5_UNORM_PACK16},    // 5_6_5
    {17, unorm, VK_FORMAT_A1R5G5B5_UNORM_PACK16},  // 1_5_5_5
    {18, unorm, VK_FORMAT_R5G5B5A1_UNORM_PACK16},  // 5_5_5_1
    {19, unorm, VK_FORMAT_B4G4R4A4_UNORM_PACK16},  // 4_4_4_4
};

}  // namespace

VkFormat color_format(std::uint32_t format, std::uint32_t number_type, std::uint32_t comp_swap) {
    for (const Row& row : rows) {
        if (row.format != format || row.number != number_type) {
            continue;
        }
        if (comp_swap == 1 && row.vk == VK_FORMAT_R8G8B8A8_UNORM) {
            return VK_FORMAT_B8G8R8A8_UNORM;
        }
        if (comp_swap == 1 && row.vk == VK_FORMAT_R8G8B8A8_SRGB) {
            return VK_FORMAT_B8G8R8A8_SRGB;
        }
        return row.vk;
    }
    return VK_FORMAT_UNDEFINED;
}

VkFormat depth_format(std::uint32_t z_format, bool stencil) {
    switch (z_format) {
    case 0:
        return stencil ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_UNDEFINED;
    case 1:
        return stencil ? VK_FORMAT_D16_UNORM_S8_UINT : VK_FORMAT_D16_UNORM;
    case 3:
        return stencil ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_D32_SFLOAT;
    default:
        return VK_FORMAT_UNDEFINED;
    }
}

}  // namespace bb::vk
