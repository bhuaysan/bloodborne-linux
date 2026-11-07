// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/video_core/renderer_vulkan/liverpool_to_vk.cpp @ff1b7a2e87eef8f301377e0cabbbeeeb73cc3b49
// Changed for bloodborne-linux, 2026-11-07: only the formats a colour target can have (the
// rows whose flags include Mrt), keyed by CB number types, without the remapped 11_11_10 and
// 10_10_10_2; B8G8R8A8 for the alternate swap; the depth table as a function; clear values
// decoded by the Vulkan format's bit layout.

#include "gpu/vk/formats.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>

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

// Where a component lies in the pixel; bits 0 for a component the format lacks.
struct Channel {
    std::uint8_t offset = 0, bits = 0;
};
using Channels = std::array<Channel, 4>;  // R, G, B, A of the Vulkan format

// The number type and the component bits of a format color_format returns.
bool layout(VkFormat vk, Number& number, Channels& ch) {
    if (vk == VK_FORMAT_B8G8R8A8_UNORM || vk == VK_FORMAT_B8G8R8A8_SRGB) {
        number = vk == VK_FORMAT_B8G8R8A8_UNORM ? unorm : srgb;
        ch = {{{16, 8}, {8, 8}, {0, 8}, {24, 8}}};
        return true;
    }
    const auto row = std::ranges::find(rows, vk, &Row::vk);
    if (row == std::end(rows)) {
        return false;
    }
    number = row->number;
    switch (row->format) {
    case 6:  // B10G11R11
        ch = {{{0, 11}, {11, 11}, {22, 10}}};
        break;
    case 9:  // A2B10G10R10
        ch = {{{0, 10}, {10, 10}, {20, 10}, {30, 2}}};
        break;
    case 14:  // only 64 bits of clear word
        ch = {{{0, 32}, {0, 32}, {0, 32}, {32, 32}}};
        break;
    case 16:  // R5G6B5
        ch = {{{11, 5}, {5, 6}, {0, 5}}};
        break;
    case 17:  // A1R5G5B5
        ch = {{{10, 5}, {5, 5}, {0, 5}, {15, 1}}};
        break;
    case 18:  // R5G5B5A1
        ch = {{{11, 5}, {6, 5}, {1, 5}, {0, 1}}};
        break;
    case 19:  // B4G4R4A4
        ch = {{{4, 4}, {8, 4}, {12, 4}, {0, 4}}};
        break;
    default: {  // 1, 2 or 4 components of one width in memory order
        constexpr std::uint8_t count[] = {0, 1, 1, 2, 1, 2, 0, 0, 0, 0, 4, 2, 4};
        constexpr std::uint8_t width[] = {0, 8, 16, 8, 32, 16, 0, 0, 0, 0, 8, 32, 16};
        ch = {};
        for (unsigned i = 0; i < count[row->format]; ++i) {
            ch[i] = {static_cast<std::uint8_t>(i * width[row->format]), width[row->format]};
        }
    }
    }
    return true;
}

std::int32_t sign_extend(std::uint32_t v, unsigned bits) {
    return static_cast<std::int32_t>(v << (32 - bits)) >> (32 - bits);
}

// A 32-bit float, a half, or an unsigned 11- or 10-bit float (5 exponent bits).
float small_float(std::uint32_t v, unsigned bits) {
    if (bits == 32) {
        return std::bit_cast<float>(v);
    }
    const int m_bits = bits == 16 ? 10 : static_cast<int>(bits) - 5;
    const std::uint32_t e = v >> m_bits & 31, m = v & ((1u << m_bits) - 1);
    const float x = e == 0    ? std::ldexp(static_cast<float>(m), -14 - m_bits)
                    : e == 31 ? (m ? NAN : INFINITY)
                              : std::ldexp(static_cast<float>(m | 1u << m_bits),
                                           static_cast<int>(e) - 15 - m_bits);
    return bits == 16 && v >> 15 ? -x : x;
}

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

VkClearColorValue color_clear_value(VkFormat format, std::uint64_t clear_word) {
    VkClearColorValue value{};
    Number number{};
    Channels ch;
    if (!layout(format, number, ch)) {
        return value;
    }
    for (unsigned i = 0; i < 4; ++i) {
        const unsigned bits = ch[i].bits;
        const auto v = static_cast<std::uint32_t>(clear_word >> ch[i].offset) &
                       static_cast<std::uint32_t>((1ull << bits) - 1);
        const float max = static_cast<float>((1ull << bits) - 1);
        if (bits == 0) {
            continue;
        } else if (number == uint) {
            value.uint32[i] = v;
        } else if (number == sint) {
            value.int32[i] = sign_extend(v, bits);
        } else if (number == sfloat) {
            value.float32[i] = small_float(v, bits);
        } else if (number == snorm) {
            const float s = static_cast<float>(sign_extend(v, bits));
            value.float32[i] = std::max(-1.0f, s / static_cast<float>((1u << (bits - 1)) - 1));
        } else if (number == srgb && i < 3) {
            const float c = static_cast<float>(v) / max;
            value.float32[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        } else {
            value.float32[i] = static_cast<float>(v) / max;
        }
    }
    return value;
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
