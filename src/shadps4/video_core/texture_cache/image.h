// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/video_core/texture_cache/image.h @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-19
// License texts: third_party/shadps4/LICENSES/

#pragma once

#include <atomic>
#include <mutex>
#include <optional>

#include "common/enum.h"
#include "common/incremental_id.h"
#include "common/lru_cache.h"
#include "common/small_vector.h"
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/host_compatibility.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/image_view.h"

namespace Vulkan {
class Instance;
class Runtime;
} // namespace Vulkan

VK_DEFINE_HANDLE(VmaAllocation)
VK_DEFINE_HANDLE(VmaAllocator)

namespace VideoCore {

enum ImageFlagBits : u32 {
    Empty = 0,
    MaybeCpuDirty = 1 << 0, ///< The page this image is in was touched before the image address
    CpuDirty = 1 << 1,      ///< Contents have been modified from the CPU
    GpuDirty = 1 << 2, ///< Image contents have been modified from the GPU (valid data in buffer
                       ///< cache)
    Dirty = MaybeCpuDirty | CpuDirty | GpuDirty,
    GpuModified = 1 << 3, ///< Contents have been modified from the GPU
    Registered = 1 << 6,  ///< True when the image is registered
};
DECLARE_ENUM_FLAG_OPERATORS(ImageFlagBits)

/// Ours (#631): a write fault on a guest thread sets CpuDirty or MaybeCpuDirty while the GPU
/// thread sets and clears other bits, so each change is one atomic read-modify-write and no
/// thread's bit is lost in another's.
class AtomicImageFlags {
public:
    AtomicImageFlags(ImageFlagBits value) : bits{value} {}
    AtomicImageFlags(const AtomicImageFlags&) = delete;
    AtomicImageFlags& operator=(const AtomicImageFlags&) = delete;

    operator ImageFlagBits() const {
        return static_cast<ImageFlagBits>(bits.load(std::memory_order_acquire));
    }
    AtomicImageFlags& operator|=(ImageFlagBits value) {
        bits.fetch_or(value, std::memory_order_acq_rel);
        return *this;
    }
    AtomicImageFlags& operator&=(ImageFlagBits value) {
        bits.fetch_and(value, std::memory_order_acq_rel);
        return *this;
    }

private:
    std::atomic<u32> bits;
};

struct UniqueImage {
    explicit UniqueImage() = default;
    explicit UniqueImage(vk::Device device, VmaAllocator allocator)
        : device{device}, allocator{allocator} {}
    ~UniqueImage();

    UniqueImage(const UniqueImage&) = delete;
    UniqueImage& operator=(const UniqueImage&) = delete;

    UniqueImage(UniqueImage&& other)
        : allocator{std::exchange(other.allocator, VK_NULL_HANDLE)},
          allocation{std::exchange(other.allocation, VK_NULL_HANDLE)},
          image{std::exchange(other.image, VK_NULL_HANDLE)}, image_ci{std::move(other.image_ci)},
          size_bytes{std::exchange(other.size_bytes, 0)} {}
    UniqueImage& operator=(UniqueImage&& other) {
        image = std::exchange(other.image, VK_NULL_HANDLE);
        allocator = std::exchange(other.allocator, VK_NULL_HANDLE);
        allocation = std::exchange(other.allocation, VK_NULL_HANDLE);
        image_ci = std::move(other.image_ci);
        size_bytes = std::exchange(other.size_bytes, 0);
        return *this;
    }

    void Create(const vk::ImageCreateInfo& image_ci);

    void Destroy();

    operator vk::Image() const {
        return image;
    }

    operator bool() const {
        return image;
    }

public:
    vk::Device device{};
    VmaAllocator allocator{};
    VmaAllocation allocation{};
    vk::Image image{};
    vk::ImageCreateInfo image_ci{};
    vk::DeviceSize size_bytes{};
};

struct Image : public Common::LRUNode<> {
    explicit Image(const Vulkan::Instance& instance, Vulkan::Runtime& runtime,
                   Common::SlotVector<ImageView>& slot_image_views, const ImageInfo& info);
    ~Image();

    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    Image(Image&&) = delete;
    Image& operator=(Image&&) = delete;

    bool Overlaps(VAddr addr, size_t size) const noexcept {
        return info.guest_address < (addr + size) && addr < (info.guest_address + info.guest_size);
    }

    vk::Image GetImage() const {
        return backing->image.image;
    }

    bool IsUntracked() {
        return track_addr == 0 || track_addr_end == 0;
    }

    /// Ours: every page of the image is write protected, so a guest write faults first. The
    /// caller holds `mutex`.
    bool IsFullyTracked() const {
        return track_addr == info.guest_address &&
               track_addr_end == info.guest_address + info.guest_size;
    }

    /// Ours (#689): before a GPU write that makes the image what it should hold, once the caller
    /// tracked it. Clears the dirty bits only while the image is still tracked, under the mutex a
    /// write fault takes: a write to the image's bytes since then untracked it whole and keeps its
    /// CpuDirty, so the next UpdateImage uploads the guest's write instead of leaving the image
    /// stale and untracked. A write to a neighbour on a shared first or last page only untracks
    /// that page; a later write to the image's bytes there is found by TrackImage's edge hashes.
    /// A multisampled image is never uploaded from guest memory, so its bits are always cleared,
    /// as upstream does.
    void ClearDirtyIfTracked() {
        std::scoped_lock lk{mutex};
        if (!IsUntracked() || info.num_samples > 1) {
            flags &= ~ImageFlagBits::Dirty;
        }
    }

    bool HasGpuValidData() const {
        return True(flags & ImageFlagBits::GpuModified) && False(flags & ImageFlagBits::Dirty);
    }

    bool CanBeDownloaded() {
        return HasGpuValidData() && info.num_samples == 1 && !info.props.has_stencil &&
               IsVulkanFormatCompatible(info.pixel_format, backing->image.image_ci.format);
    }

    void AssociateDepth(ImageId depth_image_id, u64 depth_image_uid) {
        depth_id = depth_image_id;
        depth_uid = depth_image_uid;
    }

    void DisassociateDepth() {
        depth_id = {};
        depth_uid = {};
    }

    vk::DeviceSize SizeBytes() const {
        return std::accumulate(
            backing_images.begin(), backing_images.end(), vk::DeviceSize(0),
            [](vk::DeviceSize sum, const BackingImage& bi) { return sum + bi.image.size_bytes; });
    }

    ImageView& FindView(const ImageViewInfo& view_info, bool ensure_guest_samples = true);

    using Barriers = SmallVector<vk::ImageMemoryBarrier2, 32>;
    void GetBarriers(Barriers& out_barriers, vk::ImageLayout dst_layout, vk::AccessFlags2 dst_mask,
                     vk::PipelineStageFlags2 dst_stage,
                     std::optional<SubresourceRange> subres_range = {});

public:
    Vulkan::Runtime* runtime;
    Common::SlotVector<ImageView>* slot_image_views;
    std::mutex mutex;
    ImageInfo info;
    vk::ImageAspectFlags aspect_mask = vk::ImageAspectFlagBits::eColor;
    vk::SampleCountFlags supported_samples = vk::SampleCountFlagBits::e1;
    AtomicImageFlags flags{ImageFlagBits::Dirty};
    /// The tracked range. Changed under `mutex`: a write fault on a guest thread untracks pages.
    VAddr track_addr = 0;
    VAddr track_addr_end = 0;
    ImageId depth_id{};
    u64 depth_uid{};

    vk::ImageUsageFlags usage_flags;
    vk::FormatFeatureFlags2 format_features;
    struct State {
        vk::PipelineStageFlags2 pl_stage = vk::PipelineStageFlagBits2::eAllCommands;
        vk::AccessFlags2 access_mask = vk::AccessFlagBits2::eNone;
        vk::ImageLayout layout = vk::ImageLayout::eUndefined;
    };
    struct BackingImage {
        UniqueImage image;
        State state;
        std::vector<State> subresource_states;
        SmallVector<ImageViewInfo, 2> image_view_infos;
        SmallVector<ImageViewId, 2> image_view_ids;
        u32 num_samples;
    };
    SmallVector<BackingImage, 2> backing_images;
    BackingImage* backing{};
    u64* memory_usage_counter{};
    u64 image_uid{};
    u64 lru_id{};
    u64 tick_accessed_last{};
    u64 cpu_hash{}; ///< Guarded by `mutex`: a write fault on a guest thread sets it (#631).
    u64 gpu_hash{};
    /// Ours: gpu_hash is still to be taken, and every page of the image is write protected, so its
    /// guest memory is what it was when the draw that writes the buffer over it was recorded. With
    /// gpu_hash it is guarded by `mutex`, since a fault on a guest thread takes the hash too.
    bool gpu_hash_pending{};
    u64 head_hash{};
    u64 tail_hash{};
    bool uploaded{}; ///< Set after the first upload from guest memory (profile counters only)

    struct {
        u32 texture : 1;
        u32 storage : 1;
        u32 render_target : 1;
        u32 depth_target : 1;
        u32 vo_surface : 1;
    } usage{};

    struct {
        u32 is_bound : 1;
        u32 is_target : 1;
        u32 needs_rebind : 1;
        u32 force_general : 1;
    } binding{};

private:
    static Common::IncrementalIdProvider<u64> global_image_uid;
};

} // namespace VideoCore
