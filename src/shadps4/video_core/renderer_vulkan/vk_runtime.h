// SPDX-FileCopyrightText: Copyright 2024-2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/video_core/renderer_vulkan/vk_runtime.h @11557f2eb60b3b00712c8887c30852de7615d6df
// Source: shadPS4 src/video_core/renderer_vulkan/vk_runtime.cpp @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-18
// License texts: third_party/shadps4/LICENSES/

#pragma once

#include "common/interval_set.h"
#include "common/types.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_buffer_barriers.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/types.h"

namespace VideoCore {
class BlitHelper;
} // namespace VideoCore

namespace Vulkan {

class Instance;
class Scheduler;

class Runtime {
public:
    explicit Runtime(const Instance& instance, Scheduler& scheduler);
    ~Runtime() = default;

    const Instance& GetInstance() const {
        return instance;
    }

    StagingBufferPool& GetStagingPool() {
        return staging_pool;
    }

    void TickFrame();

    void CopyBuffer(const VideoCore::Buffer* src, const VideoCore::Buffer* dst,
                    std::span<const vk::BufferCopy> copies);

    void FillBuffer(const VideoCore::Buffer* dst, u64 offset, u64 size, u32 value);

    void InlineData(VideoCore::Buffer* dst, u64 offset, u32 value);

    bool Transit(VideoCore::Image* image, vk::ImageLayout dst_layout,
                 vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access,
                 std::optional<VideoCore::SubresourceRange> subres_range = {});

    void UploadImage(VideoCore::Image* dst, const VideoCore::Buffer* src,
                     std::span<const vk::BufferImageCopy> upload_copies);
    void DownloadImage(VideoCore::Image* src, const VideoCore::Buffer* dst,
                       std::span<const vk::BufferImageCopy> download_copies);

    void CopyImage(VideoCore::Image* src, VideoCore::Image* dst);
    void CopyImageWithBuffer(VideoCore::Image* src, VideoCore::Image* dst,
                             const VideoCore::Buffer* buffer, u64 offset);
    void CopyMip(VideoCore::Image* src, VideoCore::Image* dst, u32 mip, u32 slice);

    void CopyColorAndDepth(VideoCore::Image* src, VideoCore::Image* dst);

    void CopyDepthStencil(VideoCore::Image* src, VideoCore::Image* dst,
                          const VideoCore::SubresourceRange& sub_range);

    void ResolveImage(VideoCore::Image* src, VideoCore::Image* dst,
                      const VideoCore::SubresourceRange& src_range,
                      const VideoCore::SubresourceRange& dst_range);
    void ClearImage(VideoCore::Image* dst, const VideoCore::SubresourceRange& range,
                    const vk::ClearValue& clear_value);

    void SetBackingSamples(VideoCore::Image* image, u32 num_samples, bool copy_backing = true);

    // Inline: the rasterizer calls both for every bound buffer of every draw (#593).
    void AccessBuffer(const VideoCore::Buffer* handle, u64 offset, u64 size,
                      vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access) {
        constexpr vk::AccessFlags2 READ_MASK =
            vk::AccessFlagBits2::eIndexRead | vk::AccessFlagBits2::eVertexAttributeRead |
            vk::AccessFlagBits2::eUniformRead | vk::AccessFlagBits2::eShaderRead |
            vk::AccessFlagBits2::eColorAttachmentRead |
            vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eTransferRead |
            vk::AccessFlagBits2::eMemoryRead;

        constexpr vk::AccessFlags2 WRITE_MASK =
            vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eColorAttachmentWrite |
            vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
            vk::AccessFlagBits2::eTransferWrite | vk::AccessFlagBits2::eMemoryWrite |
            vk::AccessFlagBits2::eTransformFeedbackWriteEXT;

        buffer_barriers.Access(handle->barrier_slot, offset, size, bool(src_access & READ_MASK),
                               bool(src_access & WRITE_MASK));

        memory_barrier.srcStageMask |= src_stage;
        memory_barrier.srcAccessMask |= src_access & WRITE_MASK;
    }

    bool IsBufferAccessed(const VideoCore::Buffer* handle, u64 offset, u64 size,
                          bool check_read_access = false) {
        return buffer_barriers.IsAccessed(handle->barrier_slot, offset, size, check_read_access);
    }

    void FlushBarriers();

private:
    const Instance& instance;
    Scheduler& scheduler;
    std::unique_ptr<VideoCore::BlitHelper> blit_helper;
    StagingBufferPool staging_pool;
    BufferBarrierTracker buffer_barriers;
    VideoCore::Image::Barriers image_barriers;
    vk::MemoryBarrier2 memory_barrier{};
};

// Ours (#703): the extent CopyImage copies at `mip` between images of base extents `src` and
// `dst`, clamped to both mips. Between two block-coded images (4x4 texel blocks) each side is a
// multiple of the block, or the edge of both mips, as vkCmdCopyImage requires; a side of 0 means
// the mip is not copied.
vk::Extent2D CopyMipExtent(vk::Extent2D src, vk::Extent2D dst, bool block_coded, u32 mip);

} // namespace Vulkan
