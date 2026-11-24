// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/video_core/renderer_vulkan/vk_shader_hle.cpp @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-24
// License texts: third_party/shadps4/LICENSES/

#include <atomic>
#include <cstring>
#include <utility>
#include <vector>

#include "shader_recompiler/info.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_hle.h"

namespace Vulkan {

static constexpr u64 COPY_SHADER_HASH = 0xfefebf9f;

CopyShaderStaging copy_shader_staging = CopyShaderStaging::on;

static constexpr vk::DeviceSize MaxDistanceForMerge = 64_MB;

// Ours (#832): the copies go from staging that holds only their source bytes, read from guest
// memory, to the destinations. Today's path syncs the whole span of the sources first: in
// yharnam-route at 120 that uploaded about 7.9 MB a frame for 1.3 MB copied and took 1.9 ms of
// GPU time, the copies another 0.9 ms in about 5250 regions. Staged, the regions whose
// destinations follow each other merge (about 530 a frame), and the source pages keep their
// tracking. The caller has checked that the GPU modified no source page, so guest memory holds
// what a sync would have put into the buffers.
static void CopyFromStaging(const AmdGpu::Buffer& src_sharp, const AmdGpu::Buffer& dst_sharp,
                            std::span<const CopyRange> copies, Rasterizer& rasterizer) {
    auto& runtime = rasterizer.GetRuntime();
    auto& buffer_cache = rasterizer.GetBufferCache();

    static std::vector<std::pair<u64, u64>> sources;
    static std::vector<CopyRange> regions;
    const u64 total = PackCopies(copies, sources, regions);
    if (total == 0) {
        return;
    }
    for (auto& [offset, size] : sources) {
        offset += src_sharp.base_address;
    }
    const auto staging = buffer_cache.StageGuestMemory(sources, total);

    // The destinations are obtained in spans of at most MaxDistanceForMerge, as today, and the
    // regions that land in one buffer go in one copy.
    static std::vector<vk::BufferCopy> vk_copies;
    vk_copies.clear();
    const VideoCore::Buffer* pending_buf = nullptr;
    const auto flush = [&] {
        if (!vk_copies.empty()) {
            runtime.CopyBuffer(staging.buffer, pending_buf, vk_copies);
            vk_copies.clear();
        }
    };
    for (size_t start = 0, end = 0; start < regions.size(); start = end) {
        u64 dst_min = regions[start].dst;
        u64 dst_max = regions[start].dst + regions[start].size;
        for (end = start + 1; end < regions.size(); ++end) {
            const u64 new_min = std::min(dst_min, regions[end].dst);
            const u64 new_max = std::max(dst_max, regions[end].dst + regions[end].size);
            if (new_max - new_min > MaxDistanceForMerge) {
                break;
            }
            dst_min = new_min;
            dst_max = new_max;
        }
        const auto [dst_buf, dst_buf_offset] =
            buffer_cache.ObtainBuffer(dst_sharp.base_address + dst_min, dst_max - dst_min,
                                      VideoCore::ObtainBufferFlags::IgnoreStreamBuffer);
        if (dst_buf != pending_buf) {
            flush();
            pending_buf = dst_buf;
        }
        for (size_t i = start; i < end; ++i) {
            vk_copies.push_back(vk::BufferCopy{
                .srcOffset = staging.offset + regions[i].src,
                .dstOffset = regions[i].dst - dst_min + dst_buf_offset,
                .size = regions[i].size,
            });
        }
    }
    flush();
}

// Ours (#832): --check-copy-shader-staging. Today's path did the copies; this compares the source
// bytes they read from the buffer with guest memory, read before the sync at the same point of
// the command stream, once the GPU is done.
struct StagingCheck {
    std::atomic<u64> calls{};
    std::atomic<u64> bytes{};
    std::atomic<u64> mismatched_bytes{};
    std::atomic<u64> mismatched_copies{};
};
static StagingCheck staging_check;

static void CheckSourceBytes(const VideoCore::Buffer* src_buf, VAddr src_base,
                             std::span<const vk::BufferCopy> vk_copies,
                             std::span<const CopyRange> copies, Rasterizer& rasterizer) {
    auto& runtime = rasterizer.GetRuntime();
    auto& staging_pool = runtime.GetStagingPool();
    auto& buffer_cache = rasterizer.GetBufferCache();
    u64 total = 0;
    for (const auto& copy : copies) {
        total += copy.size;
    }
    if (total == 0) {
        return;
    }
    std::vector<u8> expected(total);
    std::vector<vk::BufferCopy> reads;
    reads.reserve(copies.size());
    u64 packed = 0;
    for (size_t i = 0; i < copies.size(); ++i) {
        buffer_cache.ReadGuestMemory(src_base + copies[i].src, expected.data() + packed,
                                     copies[i].size);
        reads.push_back({vk_copies[i].srcOffset, packed, copies[i].size});
        packed += copies[i].size;
    }
    const auto readback =
        staging_pool.Request(total, VideoCore::MemoryType::HostCached, 16, true);
    for (auto& read : reads) {
        read.dstOffset += readback.offset;
    }
    runtime.CopyBuffer(src_buf, readback.buffer, reads);
    std::vector<CopyRange> checked{copies.begin(), copies.end()};
    rasterizer.GetScheduler().DeferOperation(
        [&staging_pool, readback, src_base, expected = std::move(expected),
         checked = std::move(checked)] {
            readback.Invalidate();
            u64 offset = 0;
            for (const auto& copy : checked) {
                u64 differ = 0;
                for (u64 b = 0; b < copy.size; ++b) {
                    differ += readback.mapped[offset + b] != expected[offset + b];
                }
                if (differ != 0) {
                    const u64 n = staging_check.mismatched_copies.fetch_add(1);
                    staging_check.mismatched_bytes.fetch_add(differ);
                    if (n < 20) {
                        LOG_WARNING(Render_Vulkan,
                                    "copy shader staging check: {} of {} source bytes at {:#x} "
                                    "differ from guest memory",
                                    differ, copy.size, src_base + copy.src);
                    }
                }
                offset += copy.size;
            }
            staging_check.bytes.fetch_add(offset);
            staging_pool.FreeDeferred(readback);
        });
}

static bool ExecuteCopyShaderHLE(const Shader::Info& info, const AmdGpu::ComputeProgram& cs_program,
                                 Rasterizer& rasterizer) {
    auto& runtime = rasterizer.GetRuntime();
    auto& buffer_cache = rasterizer.GetBufferCache();

    // Copy shader defines three formatted buffers as inputs: control, source, and destination.
    const auto ctl_buf_sharp = info.buffers[0].GetSharp(info);
    const auto src_buf_sharp = info.buffers[1].GetSharp(info);
    const auto dst_buf_sharp = info.buffers[2].GetSharp(info);
    const auto buf_stride = src_buf_sharp.GetStride();
    ASSERT(buf_stride == dst_buf_sharp.GetStride());

    struct CopyShaderControl {
        u32 dst_idx;
        u32 src_idx;
        u32 end;
    };
    static_assert(sizeof(CopyShaderControl) == 12);
    ASSERT(ctl_buf_sharp.GetStride() == sizeof(CopyShaderControl));
    const auto ctl_buf = reinterpret_cast<const CopyShaderControl*>(ctl_buf_sharp.base_address);

    static std::vector<vk::BufferCopy> copies;
    copies.clear();
    copies.reserve(cs_program.dim_x);

    for (u32 i = 0; i < cs_program.dim_x; i++) {
        const auto& [dst_idx, src_idx, end] = ctl_buf[i];
        const u32 local_dst_offset = dst_idx * buf_stride;
        const u32 local_src_offset = src_idx * buf_stride;
        const u32 local_size = (end + 1) * buf_stride;
        copies.emplace_back(local_src_offset, local_dst_offset, local_size);
    }

    // Ours (#832): staged when the GPU modified no source page. In yharnam-route no call's
    // sources did; today's path stays for those that do and as the switch to compare.
    static std::vector<CopyRange> ranges;
    ranges.clear();
    bool staged = copy_shader_staging != CopyShaderStaging::off;
    if (staged) {
        ranges.reserve(copies.size());
        for (const auto& copy : copies) {
            ranges.push_back({copy.srcOffset, copy.dstOffset, copy.size});
            if (buffer_cache.IsRegionGpuModified(src_buf_sharp.base_address + copy.srcOffset,
                                                 copy.size)) {
                staged = false;
                break;
            }
        }
    }
    const bool check = staged && copy_shader_staging == CopyShaderStaging::check;
    if (staged && !check) {
        CopyFromStaging(src_buf_sharp, dst_buf_sharp, ranges, rasterizer);
        copies.clear();
    }

    u32 batch_start = 0;
    u32 batch_end = 0;

    while (batch_end < copies.size()) {
        // Place first copy into the current batch
        const auto& copy = copies[batch_start];
        auto src_offset_min = copy.srcOffset;
        auto src_offset_max = copy.srcOffset + copy.size;
        auto dst_offset_min = copy.dstOffset;
        auto dst_offset_max = copy.dstOffset + copy.size;

        for (++batch_end; batch_end < copies.size(); batch_end++) {
            // Compute new src and dst bounds if we were to batch this copy
            const auto& [src_offset, dst_offset, size] = copies[batch_end];
            auto new_src_offset_min = std::min(src_offset_min, src_offset);
            auto new_src_offset_max = std::max(src_offset_max, src_offset + size);
            if (new_src_offset_max - new_src_offset_min > MaxDistanceForMerge) {
                break;
            }

            auto new_dst_offset_min = std::min(dst_offset_min, dst_offset);
            auto new_dst_offset_max = std::max(dst_offset_max, dst_offset + size);
            if (new_dst_offset_max - new_dst_offset_min > MaxDistanceForMerge) {
                break;
            }

            // We can batch this copy
            src_offset_min = new_src_offset_min;
            src_offset_max = new_src_offset_max;
            dst_offset_min = new_dst_offset_min;
            dst_offset_max = new_dst_offset_max;
        }

        // Obtain buffers for the total source and destination ranges.
        const auto [src_buf, src_buf_offset] = buffer_cache.ObtainBuffer(
            src_buf_sharp.base_address + src_offset_min, src_offset_max - src_offset_min);
        const auto [dst_buf, dst_buf_offset] = buffer_cache.ObtainBuffer(
            dst_buf_sharp.base_address + dst_offset_min, dst_offset_max - dst_offset_min,
            VideoCore::ObtainBufferFlags::IgnoreStreamBuffer);

        // Apply found buffer base.
        const auto vk_copies = std::span{copies}.subspan(batch_start, batch_end - batch_start);
        for (auto& copy : vk_copies) {
            copy.srcOffset = copy.srcOffset - src_offset_min + src_buf_offset;
            copy.dstOffset = copy.dstOffset - dst_offset_min + dst_buf_offset;
        }
        if (check) {
            CheckSourceBytes(src_buf, src_buf_sharp.base_address, vk_copies,
                             std::span{ranges}.subspan(batch_start, batch_end - batch_start),
                             rasterizer);
        }

        // Execute buffer copies.
        LOG_TRACE(Render_Vulkan, "HLE buffer copy: src_size = {}, dst_size = {}",
                  src_offset_max - src_offset_min, dst_offset_max - dst_offset_min);
        runtime.CopyBuffer(src_buf, dst_buf, vk_copies);
        batch_start = batch_end;
    }

    // Ours: the destinations are marked GPU modified in runs of ranges that overlap or touch,
    // the same bytes as one call per entry (#676: in the clinic those calls cost about 2 % of
    // the GPU thread).
    static std::vector<std::pair<u64, u64>> dst_ranges;
    dst_ranges.clear();
    dst_ranges.reserve(cs_program.dim_x);
    for (u32 i = 0; i < cs_program.dim_x; i++) {
        const auto& [dst_idx, src_idx, end] = ctl_buf[i];
        const VAddr dst_addr = dst_buf_sharp.base_address + (dst_idx * buf_stride);
        const u32 size = (end + 1) * buf_stride;
        dst_ranges.emplace_back(dst_addr, dst_addr + size);
    }
    ForEachRangeRun(dst_ranges, [&](u64 start, u64 size) {
        buffer_cache.MarkRegionAsGpuModified(start, size);
    });

    if (check && staging_check.calls.fetch_add(1) % 10000 == 9999) {
        LOG_WARNING(Render_Vulkan,
                    "copy shader staging check: {} calls, {} source bytes compared, {} bytes in "
                    "{} copies differ",
                    staging_check.calls.load(), staging_check.bytes.load(),
                    staging_check.mismatched_bytes.load(), staging_check.mismatched_copies.load());
    }

    return true;
}

bool ExecuteShaderHLE(const Shader::Info& info, const AmdGpu::Regs& regs,
                      const AmdGpu::ComputeProgram& cs_program, Rasterizer& rasterizer) {
    switch (info.pgm_hash) {
    case COPY_SHADER_HASH:
        return ExecuteCopyShaderHLE(info, cs_program, rasterizer);
    default:
        return false;
    }
}

} // namespace Vulkan
