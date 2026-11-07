#include <bit>
#include <cmath>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "gpu/vk/formats.hpp"

namespace {

using namespace bb::vk;

TEST(Formats, ColorTargets) {
    EXPECT_EQ(color_format(10, 0, 0), VK_FORMAT_R8G8B8A8_UNORM);  // 8_8_8_8 UNORM
    EXPECT_EQ(color_format(10, 6, 0), VK_FORMAT_R8G8B8A8_SRGB);   // NUMBER_SRGB is 6 in the CB
    EXPECT_EQ(color_format(10, 6, 1), VK_FORMAT_B8G8R8A8_SRGB);   // the alternate swap
    EXPECT_EQ(color_format(10, 0, 1), VK_FORMAT_B8G8R8A8_UNORM);
    EXPECT_EQ(color_format(10, 4, 1), VK_FORMAT_R8G8B8A8_UINT);  // left to the pipeline
    EXPECT_EQ(color_format(10, 0, 2), VK_FORMAT_R8G8B8A8_UNORM);
    EXPECT_EQ(color_format(12, 7, 0), VK_FORMAT_R16G16B16A16_SFLOAT);
    EXPECT_EQ(color_format(6, 7, 0), VK_FORMAT_B10G11R11_UFLOAT_PACK32);
    EXPECT_EQ(color_format(9, 0, 0), VK_FORMAT_A2B10G10R10_UNORM_PACK32);
    EXPECT_EQ(color_format(4, 7, 0), VK_FORMAT_R32_SFLOAT);
    EXPECT_EQ(color_format(16, 0, 0), VK_FORMAT_R5G6B5_UNORM_PACK16);
    // Not a target format: no Vulkan format with the same layout, a format only textures have,
    // number types a target can't write, and the invalid format.
    EXPECT_EQ(color_format(7, 7, 0), VK_FORMAT_UNDEFINED);   // 11_11_10
    EXPECT_EQ(color_format(13, 7, 0), VK_FORMAT_UNDEFINED);  // 32_32_32
    EXPECT_EQ(color_format(4, 0, 0), VK_FORMAT_UNDEFINED);   // 32 UNORM
    EXPECT_EQ(color_format(10, 2, 0), VK_FORMAT_UNDEFINED);  // USCALED
    EXPECT_EQ(color_format(0, 0, 0), VK_FORMAT_UNDEFINED);
}

TEST(Formats, DepthTargets) {
    EXPECT_EQ(depth_format(1, false), VK_FORMAT_D16_UNORM);
    EXPECT_EQ(depth_format(1, true), VK_FORMAT_D16_UNORM_S8_UINT);
    EXPECT_EQ(depth_format(3, false), VK_FORMAT_D32_SFLOAT);
    EXPECT_EQ(depth_format(3, true), VK_FORMAT_D32_SFLOAT_S8_UINT);
    EXPECT_EQ(depth_format(0, true), VK_FORMAT_D32_SFLOAT_S8_UINT);
    EXPECT_EQ(depth_format(0, false), VK_FORMAT_UNDEFINED);
    EXPECT_EQ(depth_format(2, false), VK_FORMAT_UNDEFINED);  // Z24 has no PS4 use
}

// The clear words are one pixel as stored, read by the Vulkan format's own bit layout.
TEST(Formats, ColorClearValues) {
    auto f = [](VkFormat format, std::uint64_t word) {
        const VkClearColorValue v = color_clear_value(format, word);
        return std::vector<float>(v.float32, v.float32 + 4);
    };
    auto u = [](VkFormat format, std::uint64_t word) {
        const VkClearColorValue v = color_clear_value(format, word);
        return std::vector<std::int32_t>(v.int32, v.int32 + 4);
    };
    using F = std::vector<float>;
    using I = std::vector<std::int32_t>;
    EXPECT_EQ(f(VK_FORMAT_R8G8B8A8_UNORM, 0xFF804000), (F{0, 64 / 255.f, 128 / 255.f, 1}));
    EXPECT_EQ(f(VK_FORMAT_B8G8R8A8_UNORM, 0xFF804000), (F{128 / 255.f, 64 / 255.f, 0, 1}));
    // SRGB colour becomes linear; alpha stays as it is.
    const F srgb = f(VK_FORMAT_B8G8R8A8_SRGB, 0x80FF8000);
    EXPECT_NEAR(srgb[0], 1.0f, 1e-6f);
    EXPECT_NEAR(srgb[1], 0.2158605f, 1e-6f);
    EXPECT_EQ(srgb[2], 0.0f);
    EXPECT_EQ(srgb[3], 128 / 255.f);
    EXPECT_EQ(f(VK_FORMAT_R16G16B16A16_SFLOAT, 0x0001'7C00'C000'3C00),
              (F{1, -2, INFINITY, std::ldexp(1.0f, -24)}));
    EXPECT_EQ(f(VK_FORMAT_B10G11R11_UFLOAT_PACK32, 0x3C0 | 0x380 << 11 | 0x200u << 22),
              (F{1, 0.5f, 2, 0}));
    EXPECT_EQ(f(VK_FORMAT_R8G8_SNORM, 0x7F80), (F{-1, 1, 0, 0}));  // -128 clamps to -1
    EXPECT_EQ(f(VK_FORMAT_R5G6B5_UNORM_PACK16, 0xF81F), (F{1, 0, 1, 0}));
    EXPECT_EQ(f(VK_FORMAT_R5G5B5A1_UNORM_PACK16, 0x0001), (F{0, 0, 0, 1}));
    EXPECT_EQ(f(VK_FORMAT_R32G32B32A32_SFLOAT,
                std::uint64_t{std::bit_cast<std::uint32_t>(0.5f)} << 32 |
                    std::bit_cast<std::uint32_t>(1.0f)),
              (F{1, 1, 1, 0.5f}));
    EXPECT_EQ(u(VK_FORMAT_A2B10G10R10_UINT_PACK32, 3u << 30 | 5 << 20 | 1023 << 10 | 7),
              (I{7, 1023, 5, 3}));
    EXPECT_EQ(u(VK_FORMAT_R32G32_SINT, 0x5'FFFF'FFFF), (I{-1, 5, 0, 0}));
    EXPECT_EQ(u(VK_FORMAT_R16_SINT, 0x8000), (I{-32768, 0, 0, 0}));
    EXPECT_EQ(u(VK_FORMAT_UNDEFINED, ~0ull), (I{0, 0, 0, 0}));
}

}  // namespace
