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

}  // namespace
