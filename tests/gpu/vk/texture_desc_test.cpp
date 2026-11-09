// T# and S# decoding, the texture formats and the sampler error.

#include <array>
#include <cstdint>

#include <gtest/gtest.h>

#include "gpu/vk/texture_desc.hpp"

namespace {

using namespace bb::vk;

TEST(TextureDesc, DecodesAT) {
    // base 0x12'3456'7800 (dword 0 holds bits 8 to 39, dword 1 bits 40 and up), min LOD 0x123,
    // 8_8_8_8 (10) SRGB (9), 640x360 (width - 1 in bits 0-13, height - 1 in 14-27), dst_sel
    // z y x w = 6 5 4 7... written as x=6, y=5, z=4, w=7; levels 1 to 4, tile mode 14, 2D array.
    const std::uint32_t d0 = 0x01234567;
    const std::uint32_t d1 = 0x12 | 0x123 << 8 | 10u << 20 | 9u << 26;
    const std::uint32_t d2 = 639 | 359u << 14;
    const std::uint32_t d3 = 6 | 5 << 3 | 4 << 6 | 7 << 9 | 1 << 12 | 4 << 16 | 14 << 20 | 13u << 28;
    const std::uint32_t d4 = 3 | 703u << 13;  // depth 4 layers, pitch 704
    const std::uint32_t d5 = 1 | 2 << 13;
    const std::array<std::uint32_t, 8> words{d0, d1, d2, d3, d4, d5, 0, 0};
    const auto t = decode_texture(words);
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->address, 0x12'0123'4567ull << 8);
    EXPECT_EQ(t->data_format, 10u);
    EXPECT_EQ(t->num_format, 9u);
    EXPECT_EQ(t->width, 640u);
    EXPECT_EQ(t->height, 360u);
    EXPECT_EQ(t->pitch, 704u);
    EXPECT_EQ(t->depth, 4u);
    EXPECT_EQ(t->type, image_type_2d_array);
    EXPECT_EQ(t->base_level, 1u);
    EXPECT_EQ(t->last_level, 4u);
    EXPECT_EQ(t->tile_mode, 14u);
    EXPECT_EQ(t->base_array, 1u);
    EXPECT_EQ(t->last_array, 2u);
    EXPECT_EQ(t->dst_sel, (std::array<std::uint32_t, 4>{6, 5, 4, 7}));
}

TEST(TextureDesc, ABufferDescriptorIsNoImage) {
    const std::array<std::uint32_t, 8> v{0x1000, 0, 16, 0x00077fac, 0, 0, 0, 0};
    EXPECT_EQ(decode_texture(v), std::nullopt);
}

TEST(TextureDesc, Formats) {
    const auto rgba = texture_format(10, 0);
    ASSERT_TRUE(rgba.has_value());
    EXPECT_EQ(rgba->format, VK_FORMAT_R8G8B8A8_UNORM);
    EXPECT_EQ(rgba->bits, 32u);
    EXPECT_FALSE(rgba->blocks);
    EXPECT_EQ(texture_format(10, 9)->format, VK_FORMAT_R8G8B8A8_SRGB);
    EXPECT_EQ(texture_format(2, 7)->format, VK_FORMAT_R16_SFLOAT);
    const auto bc1 = texture_format(35, 0);
    ASSERT_TRUE(bc1.has_value());
    EXPECT_EQ(bc1->bits, 64u);
    EXPECT_TRUE(bc1->blocks);
    EXPECT_EQ(texture_format(41, 9)->format, VK_FORMAT_BC7_SRGB_BLOCK);
    EXPECT_EQ(texture_format(0, 0), std::nullopt);
    EXPECT_EQ(texture_format(10, 7), std::nullopt);  // no float 8_8_8_8
    EXPECT_EQ(texture_format(13, 7), std::nullopt);  // 96-bit elements are not tiled
}

TEST(SamplerDesc, Decodes) {
    // clamp x 2, y 1, z 6; max aniso 3; compare 3; filters: mag 1, min 0, mip 2; min LOD 1.5,
    // max LOD 13; LOD bias -0.5 (14-bit two's complement of -128); border opaque white.
    const std::uint32_t d0 = 2 | 1 << 3 | 6 << 6 | 3 << 9 | 3 << 12;
    const std::uint32_t d1 = 384 | 13u * 256 << 12;
    const std::uint32_t d2 = (0x4000 - 128) | 1u << 20 | 0u << 22 | 2u << 26;
    const std::uint32_t d3 = 2u << 30;
    const SamplerDesc s = decode_sampler(std::array<std::uint32_t, 4>{d0, d1, d2, d3});
    EXPECT_EQ(s.clamp_x, 2u);
    EXPECT_EQ(s.clamp_y, 1u);
    EXPECT_EQ(s.clamp_z, 6u);
    EXPECT_EQ(s.max_aniso, 3u);
    EXPECT_EQ(s.depth_compare, 3u);
    EXPECT_FALSE(s.force_unnormalized);
    EXPECT_EQ(s.mag_filter, 1u);
    EXPECT_EQ(s.min_filter, 0u);
    EXPECT_EQ(s.mip_filter, 2u);
    EXPECT_FLOAT_EQ(s.min_lod, 1.5f);
    EXPECT_FLOAT_EQ(s.max_lod, 13.0f);
    EXPECT_FLOAT_EQ(s.lod_bias, -0.5f);
    EXPECT_EQ(s.border_color, 2u);
    EXPECT_EQ(unsupported_sampler(s), std::nullopt);
}

TEST(SamplerDesc, ForceUnnormalizedIsAnError) {
    const SamplerDesc s = decode_sampler(std::array<std::uint32_t, 4>{1u << 15, 0, 0, 0});
    EXPECT_TRUE(s.force_unnormalized);
    const auto why = unsupported_sampler(s);
    ASSERT_TRUE(why.has_value());
    EXPECT_NE(why->find("FORCE_UNNORMALIZED"), std::string::npos);
}

}  // namespace
