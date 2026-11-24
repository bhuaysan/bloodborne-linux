// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/video_core/renderer_vulkan/vk_shader_hle.h @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-24
// License texts: third_party/shadps4/LICENSES/

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

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

/// Ours (#832): whether the copy shader HLE copies sources the GPU never wrote from staging that
/// holds only the copied bytes, read from guest memory, instead of syncing the whole source span.
/// bb-run --no-copy-shader-staging takes today's path for every call; --check-copy-shader-staging
/// takes it too and compares the source bytes it copies with guest memory.
enum class CopyShaderStaging { off, on, check };
extern CopyShaderStaging copy_shader_staging;

/// One copy of the copy shader: `size` bytes from `src` to `dst`, offsets into its buffers.
struct CopyRange {
    std::uint64_t src;
    std::uint64_t dst;
    std::uint64_t size;

    friend bool operator==(const CopyRange&, const CopyRange&) = default;
};

/// Ours (#832): packs the sources of `copies` into staging in their order. `sources` gets the
/// source runs to read, each (offset, size) and placed one after the other in staging; `regions`
/// gets the copies from staging (`src` is the offset in staging), with copies whose destinations
/// follow each other merged. Returns the staging size. Copying the regions writes the same bytes
/// as the copies in order, as long as no copy's destination overlaps another copy's source or
/// destination (undefined in one vkCmdCopyBuffer, which today's path issues too).
inline std::uint64_t PackCopies(std::span<const CopyRange> copies,
                                std::vector<std::pair<std::uint64_t, std::uint64_t>>& sources,
                                std::vector<CopyRange>& regions) {
    sources.clear();
    regions.clear();
    std::uint64_t packed = 0;
    for (const auto& copy : copies) {
        if (copy.size == 0) {
            continue;
        }
        if (!sources.empty() && sources.back().first + sources.back().second == copy.src) {
            sources.back().second += copy.size;
        } else {
            sources.emplace_back(copy.src, copy.size);
        }
        if (!regions.empty() && regions.back().dst + regions.back().size == copy.dst) {
            regions.back().size += copy.size;
        } else {
            regions.push_back({packed, copy.dst, copy.size});
        }
        packed += copy.size;
    }
    return packed;
}

/// Attempts to execute a shader using HLE if possible.
bool ExecuteShaderHLE(const Shader::Info& info, const AmdGpu::Regs& regs,
                      const AmdGpu::ComputeProgram& cs_program, Rasterizer& rasterizer);

} // namespace Vulkan
