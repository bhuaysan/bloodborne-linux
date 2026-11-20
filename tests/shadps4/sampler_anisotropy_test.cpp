// The anisotropy bb-run --anisotropy gives a sampler (VideoCore::ChooseAnisotropy, #658): the
// level reaches samplers with linear filtering and mipmaps, point-filtered samplers, those without
// mipmaps and compare samplers keep the game's value, and the default keeps the game's value for
// all. Header-only, needs no Vulkan device.

#include <gtest/gtest.h>

#include "video_core/texture_cache/sampler_anisotropy.h"

namespace {

using AmdGpu::AnisoRatio;
using AmdGpu::Filter;
using AmdGpu::MipFilter;
using VideoCore::ChooseAnisotropy;

constexpr float device_max = 16.0f;

// A trilinear sampler over 13 mip levels, as the game's ground textures use.
AmdGpu::Sampler Trilinear() {
    AmdGpu::Sampler s{};
    s.xy_mag_filter.Assign(Filter::Bilinear);
    s.xy_min_filter.Assign(Filter::Bilinear);
    s.mip_filter.Assign(MipFilter::Linear);
    s.max_lod.Assign(13 * 256);
    return s;
}

AmdGpu::Sampler Aniso(AnisoRatio ratio) {
    AmdGpu::Sampler s = Trilinear();
    s.xy_mag_filter.Assign(Filter::AnisoLinear);
    s.xy_min_filter.Assign(Filter::AnisoLinear);
    s.max_aniso.Assign(ratio);
    return s;
}

TEST(SamplerAnisotropy, DefaultKeepsTheGamesValue) {
    const auto plain = ChooseAnisotropy(Trilinear(), false, 0, true, device_max);
    EXPECT_FALSE(plain.enable);
    EXPECT_EQ(plain.max, 1.0f);
    const auto four = ChooseAnisotropy(Aniso(AnisoRatio::Four), false, 0, true, device_max);
    EXPECT_TRUE(four.enable);
    EXPECT_EQ(four.max, 4.0f);
    // Only the minification filter anisotropic is enough.
    AmdGpu::Sampler min_only = Trilinear();
    min_only.xy_min_filter.Assign(Filter::AnisoLinear);
    min_only.max_aniso.Assign(AnisoRatio::Two);
    EXPECT_EQ(ChooseAnisotropy(min_only, false, 0, true, device_max).max, 2.0f);
}

TEST(SamplerAnisotropy, LevelSetsLinearMipmappedSamplers) {
    for (const unsigned level : {2u, 4u, 8u, 16u}) {
        const auto r = ChooseAnisotropy(Trilinear(), false, level, true, device_max);
        EXPECT_TRUE(r.enable) << level;
        EXPECT_EQ(r.max, static_cast<float>(level));
        // Also above or below what the game asked for.
        const auto g = ChooseAnisotropy(Aniso(AnisoRatio::Four), false, level, true, device_max);
        EXPECT_EQ(g.max, static_cast<float>(level));
    }
    // A point mip filter still has mipmaps.
    AmdGpu::Sampler point_mip = Trilinear();
    point_mip.mip_filter.Assign(MipFilter::Point);
    EXPECT_EQ(ChooseAnisotropy(point_mip, false, 16, true, device_max).max, 16.0f);
    // 1 turns it off, also where the game asked for it.
    const auto off = ChooseAnisotropy(Aniso(AnisoRatio::Eight), false, 1, true, device_max);
    EXPECT_FALSE(off.enable);
    EXPECT_EQ(off.max, 1.0f);
}

TEST(SamplerAnisotropy, OtherSamplersKeepTheGamesValue) {
    AmdGpu::Sampler point_min = Trilinear();
    point_min.xy_min_filter.Assign(Filter::Point);
    AmdGpu::Sampler point_mag = Trilinear();
    point_mag.xy_mag_filter.Assign(Filter::AnisoPoint);
    AmdGpu::Sampler no_mips = Trilinear();
    no_mips.mip_filter.Assign(MipFilter::None);
    AmdGpu::Sampler one_level = Trilinear();
    one_level.max_lod.Assign(0);
    AmdGpu::Sampler clamped = Trilinear();
    clamped.min_lod.Assign(2 * 256);
    clamped.max_lod.Assign(2 * 256);
    for (const AmdGpu::Sampler& s : {point_min, point_mag, no_mips, one_level, clamped}) {
        EXPECT_FALSE(VideoCore::AnisotropyOverrideApplies(s, false));
        const auto r = ChooseAnisotropy(s, false, 16, true, device_max);
        EXPECT_EQ(r.enable, AmdGpu::IsAnisoFilter(s.xy_mag_filter) ||
                                AmdGpu::IsAnisoFilter(s.xy_min_filter));
        EXPECT_EQ(r.max, 1.0f);
    }
    // A depth compare keeps the game's value, on and off.
    EXPECT_FALSE(ChooseAnisotropy(Trilinear(), true, 16, true, device_max).enable);
    const auto depth = ChooseAnisotropy(Aniso(AnisoRatio::Two), true, 16, true, device_max);
    EXPECT_TRUE(depth.enable);
    EXPECT_EQ(depth.max, 2.0f);
    EXPECT_TRUE(ChooseAnisotropy(Aniso(AnisoRatio::Two), true, 1, true, device_max).enable);
}

TEST(SamplerAnisotropy, DeviceLimits) {
    EXPECT_EQ(ChooseAnisotropy(Trilinear(), false, 16, true, 8.0f).max, 8.0f);
    EXPECT_EQ(ChooseAnisotropy(Aniso(AnisoRatio::Sixteen), false, 0, true, 4.0f).max, 4.0f);
    EXPECT_FALSE(ChooseAnisotropy(Trilinear(), false, 16, false, device_max).enable);
    EXPECT_FALSE(ChooseAnisotropy(Aniso(AnisoRatio::Four), false, 0, false, device_max).enable);
}

} // namespace
