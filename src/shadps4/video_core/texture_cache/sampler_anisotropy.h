// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/video_core/texture_cache/sampler.cpp @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-20: the anisotropy choice of Sampler::Sampler moved here
// and given the level of bb-run --anisotropy
// License texts: third_party/shadps4/LICENSES/
//
// The anisotropy of a sampler: the game's own, or bb-run --anisotropy (#658). Header-only, so the
// unit test needs no Vulkan device.

#pragma once

#include <algorithm>

#include "common/types.h"
#include "video_core/amdgpu/resource.h"

namespace VideoCore {

struct SamplerAnisotropy {
    bool enable = false;
    float max = 1.0f;
};

// Whether `level` (bb-run --anisotropy) applies to `sampler`: linear magnification and
// minification, mipmaps (a mip filter and a LOD range) and no depth compare. Point-filtered
// samplers, those without mipmaps and compare samplers keep the game's value.
inline bool AnisotropyOverrideApplies(const AmdGpu::Sampler& sampler, bool is_depth) {
    const auto linear = [](AmdGpu::Filter filter) {
        return filter == AmdGpu::Filter::Bilinear || filter == AmdGpu::Filter::AnisoLinear;
    };
    return !is_depth && linear(sampler.xy_mag_filter) && linear(sampler.xy_min_filter) &&
           sampler.mip_filter != AmdGpu::MipFilter::None && sampler.MaxLod() > sampler.MinLod();
}

// `level` 0 keeps the game's value: anisotropic when the S# asks for an anisotropic filter, at its
// ratio. Otherwise samplers that AnisotropyOverrideApplies to get `level` (1 turns anisotropic
// filtering off), and the rest keep the game's value. Always clamped to the device's maximum, and
// off without the device feature.
inline SamplerAnisotropy ChooseAnisotropy(const AmdGpu::Sampler& sampler, bool is_depth, u32 level,
                                          bool supported, float device_max) {
    if (!supported) {
        return {};
    }
    if (level != 0 && AnisotropyOverrideApplies(sampler, is_depth)) {
        if (level <= 1) {
            return {};
        }
        return {true, std::clamp(static_cast<float>(level), 1.0f, device_max)};
    }
    if (!AmdGpu::IsAnisoFilter(sampler.xy_mag_filter) &&
        !AmdGpu::IsAnisoFilter(sampler.xy_min_filter)) {
        return {};
    }
    return {true, std::clamp(sampler.MaxAniso(), 1.0f, device_max)};
}

} // namespace VideoCore
