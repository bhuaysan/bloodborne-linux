// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/video_core/renderer_vulkan/vk_shader_hle.h @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-19
// License texts: third_party/shadps4/LICENSES/

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace AmdGpu {
struct ComputeProgram;
union Regs;
} // namespace AmdGpu

namespace Shader {
struct Info;
}

namespace Vulkan {

class Rasterizer;

/// Ours: calls `func(start, size)` once for each run of consecutive ranges in `ranges`, each
/// [start, end), that overlap or touch the run before them: the same bytes as the ranges, in
/// fewer calls when they come in order (#676). Ranges out of order are not sorted, which costs
/// more than the calls it saves; they only start a new run.
template <typename Func>
void ForEachRangeRun(std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges,
                     Func&& func) {
    for (std::size_t i = 0; i < ranges.size();) {
        std::uint64_t start = ranges[i].first;
        std::uint64_t end = ranges[i].second;
        for (++i; i < ranges.size() && ranges[i].first <= end && ranges[i].second >= start; ++i) {
            start = std::min(start, ranges[i].first);
            end = std::max(end, ranges[i].second);
        }
        func(start, end - start);
    }
}

/// Attempts to execute a shader using HLE if possible.
bool ExecuteShaderHLE(const Shader::Info& info, const AmdGpu::Regs& regs,
                      const AmdGpu::ComputeProgram& cs_program, Rasterizer& rasterizer);

} // namespace Vulkan
