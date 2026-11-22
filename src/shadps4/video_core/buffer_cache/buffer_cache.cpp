// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/video_core/buffer_cache/buffer_cache.cpp @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-22
// License texts: third_party/shadps4/LICENSES/

#include <algorithm>
#include <thread>
#include <magic_enum/magic_enum.hpp>
#include <xxhash.h>

#include "common/alignment.h"
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/buffer_cache/memory_tracker.h"
#include "video_core/buffer_cache/region_definitions.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

#include <vk_mem_alloc.h>
#include "common/frame_profile.hpp"

namespace VideoCore {

static constexpr size_t GDS_BUFFER_SIZE = 64_KB;
static constexpr size_t NULL_INDEX_BUFFER_SIZE = 16;
static constexpr size_t STREAM_BUFFER_SIZE = 128_MB;

// Ours (#777): uploads from this size on are copied on the scheduler's copy threads. In
// yharnam-route at 60 they carry almost all of the memmove time (about 1000 a second, 350 KiB
// on average); smaller ones take about a microsecond, as long as the hand-over.
static constexpr size_t AsyncSyncBytes = 64_KB;

// The memmoves of one upload, run on a copy thread.
struct CopyJob {
    struct Range {
        VAddr guest;
        u64 offset; // in the staging space
        u64 size;
    };
    Vulkan::StagingBufferRef staging;
    boost::container::small_vector<Range, 4> ranges;
};
constexpr std::optional<u32> CmaskColorExpandedValue(u32 num_samples) {
    switch (num_samples) {
    case 1:
        return 0xFFFFFFFF;
    case 2:
        return 0xDDDDDDDD;
    case 4:
        return 0xEEEEEEEE;
    case 8:
        return 0xFFFFFFFF;
    default:
        return 0;
    }
}
static_assert(CmaskColorExpandedValue(1) == 0xFFFFFFFF);
static_assert(CmaskColorExpandedValue(2) == 0xDDDDDDDD);
static_assert(CmaskColorExpandedValue(4) == 0xEEEEEEEE);
static_assert(CmaskColorExpandedValue(8) == 0xFFFFFFFF);

static constexpr auto ARENA_USAGE =
    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
    vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
    vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eVertexBuffer |
    vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress;

std::optional<u32> FindMemoryType(const vk::PhysicalDeviceMemoryProperties& properties,
                                  vk::MemoryPropertyFlags wanted, u32 memory_type_bits) {
    for (u32 i = 0; i < properties.memoryTypeCount; ++i) {
        if (((memory_type_bits >> i) & 1) == 0) {
            continue;
        }
        const auto flags = properties.memoryTypes[i].propertyFlags;
        if ((flags & wanted) == wanted) {
            return i;
        }
    }
    return std::nullopt;
}

BufferCache::BufferCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                         Vulkan::Runtime& runtime_, AmdGpu::Liverpool* liverpool_,
                         TextureCache& texture_cache_, PageManager& tracker,
                         Core::MemoryManager& memory_)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_},
      staging_pool{runtime_.GetStagingPool()}, liverpool{liverpool_},
      memory{memory_}, texture_cache{texture_cache_},
      memory_tracker{std::make_unique<MemoryTracker>(tracker)},
      // Ours: the stream buffer lives in cached host memory, not in device-local memory the CPU
      // writes through write combining (upstream's Stream type). The GPU thread copies a few
      // small buffers into it per draw, and every locked instruction after such a copy (the
      // driver's reference counts, mutexes) waited for the write-combining buffers to drain: at
      // 120 FPS in the clinic the GPU thread's frame took 1.4 ms (12 %) longer (#676).
      stream_buffer{instance, scheduler, MemoryType::HostCached, STREAM_BUFFER_SIZE},
      gds_buffer{instance, 0, GDS_BUFFER_SIZE, MemoryType::Stream, "GDS Buffer"},
      memory_semaphore{instance} {
    const vk::BufferCreateInfo probe_ci = {
        .flags =
            vk::BufferCreateFlagBits::eSparseBinding | vk::BufferCreateFlagBits::eSparseResidency,
        .size = ARENA_PAGE_SIZE,
        .usage = SupportedUsage(instance, ARENA_USAGE),
        .sharingMode = vk::SharingMode::eExclusive,
    };
    const vk::DeviceBufferMemoryRequirements req_info = {
        .pCreateInfo = &probe_ci,
    };
    const auto device = instance.GetDevice();
    const auto reqs = device.getBufferMemoryRequirements(req_info).memoryRequirements;
    block_size = Common::AlignUp(std::max<u64>(reqs.alignment, MIN_BLOCK_SIZE), reqs.alignment);
    ASSERT_MSG(std::popcount(block_size) == 1, "Sparse block size {} is not a power of 2",
               block_size);
    block_shift = std::bit_width(block_size) - 1;
    blocks_per_arena_page = ARENA_PAGE_SIZE / block_size;
    blocks_per_arena_page_shift = ARENA_PAGE_BITS - block_shift;
    arena_memory_type_index =
        FindMemoryType(instance.GetMemoryProperties(), vk::MemoryPropertyFlagBits::eDeviceLocal,
                       reqs.memoryTypeBits)
            .value();

    if (!instance.IsMaintenance6Supported()) {
        null_index_buffer = std::make_unique<Buffer>(instance, 0, NULL_INDEX_BUFFER_SIZE,
                                                     MemoryType::DeviceLocal, "Null Index Buffer");
        runtime.FillBuffer(null_index_buffer.get(), 0u, NULL_INDEX_BUFFER_SIZE, 0u);
    }
}

// Ours (#777): the copy jobs write into the staging pool's buffers, which go before the scheduler.
BufferCache::~BufferCache() {
    scheduler.Copies().WaitIdle();
}

// With readbacks off (upstream's default and the reference's setting), upstream marks the range CPU
// modified and nothing more. The next sync then uploads the whole page from guest memory, also over
// the bytes the GPU wrote there, which guest memory never got: a vertex buffer the GPU wrote next
// to data the CPU writes later loses its vertices, and the game draws flat polygons (#537, after
// a respawn). Upstream's Relaxed mode downloads those bytes first, which this keeps: the parts the
// GPU did not modify are marked at once, the rest on the GPU thread right after the download, so
// no GPU write comes between the two.
void BufferCache::InvalidateMemory(VAddr device_addr, u64 size) {
    memory_tracker->InvalidateRegion(device_addr, size, [this, device_addr, size] {
        OnGpuThread([this, device_addr, size] {
            FlushGpuModified(device_addr, size);
            memory_tracker->MarkRegionAsCpuModified(device_addr, size);
        });
    });
}

// How many pages a write fault makes writable at most, the faulting one and those after it. The
// guest writes most of its per-frame data in runs of 16 pages (64 KiB); in a fault trace of
// yharnam-route's play-walk this many cut the faults to 13 % for about 2 pages marked CPU modified
// and not written per fault.
constexpr u64 FAULT_PAGES = 16;

// As InvalidateMemory: a page the GPU wrote is downloaded and then marked on the GPU thread.
void BufferCache::InvalidateFromFault(VAddr device_addr) {
    if (memory_tracker->MarkCpuModifiedFromFault(device_addr, FAULT_PAGES, false)) {
        return;
    }
    OnGpuThread([this, device_addr] {
        FlushGpuModified(device_addr & ~(BYTES_PER_PAGE - 1), BYTES_PER_PAGE);
        memory_tracker->MarkCpuModifiedFromFault(device_addr, FAULT_PAGES, true);
    });
}

// Ours: nothing reads the range's old contents again, so nothing is downloaded. Its GPU state goes
// too, so that a later download in the same window can't write the old bytes over what the range
// holds next.
void BufferCache::DropMemory(VAddr device_addr, u64 size) {
    if (!memory_tracker->IsRegionGpuModified(device_addr, size)) {
        // Only a GPU write into memory the game unmaps could set a bit after this check.
        memory_tracker->MarkRegionAsCpuModified(device_addr, size);
        return;
    }
    OnGpuThread([this, device_addr, size] { UnmarkRegionAsGpuModified(device_addr, size, true); });
}

void BufferCache::OnGpuThread(auto&& func) {
    if (liverpool == nullptr ||
        std::this_thread::get_id() == liverpool->GetGpuCommandProcessorThread()) {
        func();
    } else {
        liverpool->SendCommand<true>(func);
    }
}

// Runs on the GPU thread. A download queued earlier (ReadMemory) has dropped its ranges from
// gpu_modified_ranges but writes guest memory only once its copy is done, so it is waited for and
// its write run first (Finish waits, but runs only the operations done before its submit).
void BufferCache::FlushGpuModified(VAddr device_addr, u64 size) {
    scheduler.Finish();
    scheduler.PopPendingOperations();
    DownloadWindow(device_addr, size, false);
}

void BufferCache::UnmarkRegionAsGpuModified(VAddr device_addr, u64 size, bool is_write) {
    memory_tracker->UnmarkRegionAsGpuModified(device_addr, size, is_write);
    gpu_modified_ranges.Subtract(device_addr, size);
}

void BufferCache::ReadMemory(VAddr device_addr, u64 size) {
    DownloadWindow(device_addr, size, true);
}

void BufferCache::DownloadWindow(VAddr device_addr, u64 size, bool async) {
    {
        const u32 first_block = device_addr >> block_shift;
        const u32 last_block = (device_addr + size - 1) >> block_shift;
        const auto* arena = GetArena(first_block, last_block);

        // GPU-modified ranges come as many small scattered islands,
        // so the download is widened to a window around the request
        constexpr u64 WindowSize = 512_KB;
        const VAddr arena_end = arena->cpu_addr + arena->size_bytes;
        const VAddr window_start =
            std::max<VAddr>(Common::AlignDown(device_addr, WindowSize), arena->cpu_addr);
        const VAddr window_end = std::min<VAddr>(
            std::max<VAddr>(window_start + WindowSize, device_addr + size), arena_end);
        DownloadMemory(arena, window_start, window_end - window_start, async);
    }
}

void BufferCache::DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size, bool async) {
    boost::container::small_vector<vk::BufferCopy, 1> copies;
    u64 total_size_bytes = 0;
    const VAddr arena_base = arena->cpu_addr;
    memory_tracker->ForEachDownloadRange<false>(device_addr, size, [&](u64 address, u64 size) {
        const auto add_download = [&](VAddr start, VAddr end) {
            const u64 new_offset = start - arena_base;
            const u64 new_size = end - start;
            copies.push_back(vk::BufferCopy{
                .srcOffset = new_offset,
                .dstOffset = total_size_bytes,
                .size = new_size,
            });
            // Align up to avoid cache conflicts
            constexpr u64 align = 64ULL;
            constexpr u64 mask = ~(align - 1ULL);
            total_size_bytes += (new_size + align - 1) & mask;
        };
        gpu_modified_ranges.ForEachInRange(address, size, add_download);
        gpu_modified_ranges.Subtract(address, size);
    });
    if (total_size_bytes == 0) {
        return;
    }
    const auto download =
        staging_pool.Request(total_size_bytes, VideoCore::MemoryType::HostCached, 16, async);
    for (auto& copy : copies) {
        copy.dstOffset += download.offset;
    }
    runtime.CopyBuffer(arena, download.buffer, copies);

    const auto write_data = [this, arena_base, device_addr, size, download, async,
                             copies = std::move(copies)] {
        download.Invalidate();
        for (const auto& copy : copies) {
            auto* dst_addr = std::bit_cast<u8*>(arena_base + copy.srcOffset);
            memory.TryWriteBacking(dst_addr, download.mapped + (copy.dstOffset - download.offset),
                                    copy.size);
        }
        memory_tracker->UnmarkRegionAsGpuModified(device_addr, size, false);

        if (async) {
            staging_pool.FreeDeferred(download);
        }
    };

    if (async) {
        scheduler.DeferOperation(std::move(write_data));
    } else {
        scheduler.Finish();
        write_data();
    }
}

std::pair<const Buffer*, u64> BufferCache::ObtainBuffer(VAddr device_addr, u32 size,
                                                        ObtainBufferFlags flags) {
    bb::common::frame_profile::Scope profile_buffer(bb::common::frame_profile::Part::buffer);
    const bool is_written = True(flags & ObtainBufferFlags::IsWritten);
    const bool is_texel_buffer = True(flags & ObtainBufferFlags::IsTexelBuffer);
    const bool skip_stream_buffer = True(flags & ObtainBufferFlags::IgnoreStreamBuffer);
    // For read-only buffers use the stream buffer to reduce renderpass breaks (ours: in cached
    // host memory, see the constructor).
    if (!is_written && !skip_stream_buffer && size <= STREAM_THRESHOLD &&
        !IsRegionGpuModified(device_addr, size)) {
        const auto [data, offset] = stream_buffer.Map(size, instance.UniformMinAlignment());
        memory.CopySparseMemory(device_addr, data, size);
        stream_buffer.Commit();
        return {&stream_buffer, offset};
    }
    const u64 first_block = device_addr >> block_shift;
    const u64 last_block = (device_addr + size - 1) >> block_shift;
    const auto* arena = GetArena(first_block, last_block);
    EnsureResident(arena, first_block, last_block);
    if (is_written) {
        texture_cache.MarkAsMaybeGpuDirty(device_addr, size);
    }
    SynchronizeMemory(arena, device_addr, size, is_written, is_texel_buffer);
    if (is_written) {
        gpu_modified_ranges.Add(device_addr, size);
    }
    return {arena, arena->Offset(device_addr)};
}

std::pair<const Buffer*, u64> BufferCache::ObtainBufferForImage(VAddr device_addr, u32 size) {
    if (IsRegionGpuModified(device_addr, size)) {
        return ObtainBuffer(device_addr, size);
    }
    const auto staging = staging_pool.Request(size, VideoCore::MemoryType::HostUncached,
                                              instance.StorageMinAlignment());
    memory.CopySparseMemory(device_addr, staging.mapped, staging.size);
    staging.Flush();
    return {staging.buffer, staging.offset};
}

bool BufferCache::IsRegionCpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionCpuModified(addr, size);
}

bool BufferCache::IsRegionGpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionGpuModified(addr, size);
}

void BufferCache::MarkRegionAsGpuModified(VAddr addr, size_t size) {
    texture_cache.MarkAsMaybeGpuDirty(addr, size);
    memory_tracker->MarkRegionAsGpuModified(addr, size);
    gpu_modified_ranges.Add(addr, size);
}

void BufferCache::ReadEdgeImagePages(const Image& image) {
    // May happen that after downloading the image and invalidating region,
    // that there were GPU modified ranges that are lost due to CPU reuploading.
    // This doesn't change tracker state and it is spected to call DownloadImageMemory after this.
    const VAddr image_addr = image.info.guest_address;
    const u64 image_size = image.info.guest_size;
    const VAddr image_end = image_addr + image_size;
    const VAddr page_start = PageManager::GetPageAddr(image_addr);
    const VAddr page_end = PageManager::GetNextPageAddr(image_end - 1);
    boost::container::small_vector<vk::BufferCopy, 2> copies;
    u64 total_size_bytes = 0;
    const auto [buffer, offset] = ObtainBuffer(page_start, page_end - page_start);
    const auto add_download = [&](VAddr start, VAddr end) {
        const u64 new_offset = start - buffer->CpuAddr();
        const u64 new_size = end - start;
        copies.push_back(vk::BufferCopy{
            .srcOffset = new_offset,
            .dstOffset = total_size_bytes,
            .size = new_size,
        });
        // Align up to avoid cache conflicts
        constexpr u64 align = 64ULL;
        constexpr u64 mask = ~(align - 1ULL);
        total_size_bytes += (new_size + align - 1) & mask;
    };
    gpu_modified_ranges.ForEachInRange(page_start, image_addr - page_start, add_download);
    gpu_modified_ranges.ForEachInRange(image_end, page_end - image_end, add_download);
    gpu_modified_ranges.Subtract(page_start, page_end - page_start);
    if (total_size_bytes == 0) {
        return;
    }
    const auto download =
        staging_pool.Request(total_size_bytes, VideoCore::MemoryType::HostCached, 16, true);
    for (auto& copy : copies) {
        // Modify copies to have the staging offset in mind
        copy.dstOffset += download.offset;
    }
    scheduler.EndRendering();
    runtime.CopyBuffer(buffer, download.buffer, copies);
    scheduler.DeferOperation(
        [this, base = buffer->CpuAddr(), download, copies = std::move(copies)]() {
            download.Invalidate();
            for (const auto& copy : copies) {
                const VAddr copy_device_addr = base + copy.srcOffset;
                const u64 dst_offset = copy.dstOffset - download.offset;
                memory.TryWriteBacking(std::bit_cast<u8*>(copy_device_addr),
                                        download.mapped + dst_offset, copy.size);
            }
            staging_pool.FreeDeferred(download);
        });
}

const Buffer* BufferCache::GetArena(u64 first_block, u64 last_block) {
    const u64 first_page = first_block >> blocks_per_arena_page_shift;
    const u64 last_page = last_block >> blocks_per_arena_page_shift;
    ASSERT_MSG(last_page - first_page <= 1,
               "Buffer request cannot span more than two VA arena pages");

    const auto* first_arena = address_space[first_page];
    const auto* last_arena = address_space[last_page];
    if (first_arena == last_arena) {
        if (!first_arena) {
            const u64 base_block = Common::AlignDownPow2<u64>(first_block, blocks_per_arena_page);
            const u64 num_pages = last_page - first_page + 1;
            const auto* new_arena =
                &arenas.emplace_back(instance, base_block << block_shift,
                                     num_pages << ARENA_PAGE_BITS, MemoryType::Sparse);
            address_space[first_page] = new_arena;
            address_space[last_page] = new_arena;
        }
        return address_space[first_page];
    }

    LOG_WARNING(Render, "Migrating arena");

    const u64 first_addr = first_arena ? first_arena->cpu_addr : (first_page << ARENA_PAGE_BITS);
    const u64 first_size = first_arena ? first_arena->size_bytes : ARENA_PAGE_SIZE;
    const u64 last_size = last_arena ? last_arena->size_bytes : ARENA_PAGE_SIZE;

    const u64 base_block = first_addr >> block_shift;
    const u64 total_size = first_size + last_size;
    const u64 end_block = (first_addr + total_size) >> block_shift;
    auto* new_arena = &arenas.emplace_back(instance, first_addr, total_size, MemoryType::Sparse);
    auto* bind = BindsForArena(new_arena);
    resident_ranges.ForEachInRange(base_block, end_block, [&](const Backing& backing) {
        const u64 start = std::max(base_block, backing.start);
        const u64 end = std::min(end_block, backing.end);
        bind->binds.push_back(vk::SparseMemoryBind{
            .resourceOffset = (start - base_block) << block_shift,
            .size = (end - start) << block_shift,
            .memory = backing.memory,
            .memoryOffset = (backing.offset + start - backing.start) << block_shift,
        });
    });

    u64 base_page = first_addr >> ARENA_PAGE_BITS;
    for (u32 page = 0; page < (first_size >> ARENA_PAGE_BITS); ++page) {
        address_space[base_page + page] = new_arena;
    }
    base_page = last_page;
    for (u32 page = 0; page < (last_size >> ARENA_PAGE_BITS); ++page) {
        address_space[base_page + page] = new_arena;
    }
    return new_arena;
}

void BufferCache::EnsureResident(const Buffer* arena, u64 first_block, u64 last_block) {
    bb::common::frame_profile::Scope profile(bb::common::frame_profile::Part::resident);
    u32 resident_blocks{};
    IntervalList bind_ranges;
    resident_ranges.ForEachGap(first_block, last_block + 1, [&](u64 start, u64 end) {
        resident_blocks += end - start;
        bind_ranges.Add({start, end});
    });

    if (bind_ranges.Empty()) {
        return;
    }

    const bool has_bda = instance.IsBufferDeviceAddressSupported();
    const vk::MemoryAllocateFlagsInfo alloc_flags = {
        .flags = vk::MemoryAllocateFlagBits::eDeviceAddress,
    };
    const vk::MemoryAllocateInfo alloc_info = {
        .pNext = has_bda ? &alloc_flags : nullptr,
        .allocationSize = resident_blocks << block_shift,
        .memoryTypeIndex = arena_memory_type_index,
    };
    const auto device_memory = Vulkan::Check(instance.GetDevice().allocateMemory(alloc_info));

    u64 memory_offset{};
    ArenaBinds* binds = BindsForArena(arena);
    for (const auto& range : bind_ranges) {
        Backing backing;
        backing.start = range.start;
        backing.end = range.end;
        backing.memory = device_memory;
        backing.offset = memory_offset >> block_shift;
        resident_ranges.Add(backing);

        LOG_INFO(Render, "Making range start={}, end={} resident", backing.start, backing.end);

        const auto& bind = binds->binds.emplace_back(vk::SparseMemoryBind{
            .resourceOffset = (range.start << block_shift) - arena->cpu_addr,
            .size = (range.end - range.start) << block_shift,
            .memory = device_memory,
            .memoryOffset = memory_offset,
        });
        memory_offset += bind.size;
    }
}

bool BufferCache::SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size,
                                    bool is_written, bool is_texel_buffer) {
    bb::common::frame_profile::Scope profile(bb::common::frame_profile::Part::sync);
    boost::container::small_vector<vk::BufferCopy, 4> copies;
    size_t total_size_bytes{};
    memory_tracker->ForEachUploadRange(device_addr, size, is_written, [&](u64 addr, u64 size) {
        copies.emplace_back(total_size_bytes, addr, size);
        total_size_bytes += size;
    });
    if (!copies.empty()) {
        const auto staging = staging_pool.Request(total_size_bytes, MemoryType::HostUncached);
        // Ours (#777): a large upload's memmove goes to the scheduler's copy threads, and the
        // replay of the copy below waits for it. The pages were protected again above, so a CPU
        // write during the job marks them modified and the next draw syncs them again.
        if (total_size_bytes >= AsyncSyncBytes) {
            CopyJob job{.staging = staging};
            job.ranges.reserve(copies.size());
            for (const auto& copy : copies) {
                job.ranges.push_back({copy.dstOffset, copy.srcOffset, copy.size});
            }
            scheduler.Copies().Push([this, job = std::move(job)] {
                for (const auto& range : job.ranges) {
                    memory.CopySparseMemory(range.guest, job.staging.mapped + range.offset,
                                            range.size);
                }
                job.staging.Flush();
            });
        } else {
            for (const auto& copy : copies) {
                memory.CopySparseMemory(copy.dstOffset, staging.mapped + copy.srcOffset,
                                        copy.size);
            }
            staging.Flush();
        }
        for (auto& copy : copies) {
            copy.srcOffset += staging.offset;
            copy.dstOffset -= arena->cpu_addr;
        }
        runtime.CopyBuffer(staging.buffer, arena, copies);
    }
    if (is_texel_buffer && !is_written) {
        return SynchronizeMemoryFromImage(arena, device_addr, size);
    }
    return false;
}

bool BufferCache::SynchronizeMemoryFromImage(const Buffer* arena, VAddr device_addr, u32 size) {
    if (const auto meta = texture_cache.GetMetaInfo(device_addr)) {
        if (meta->type == TextureCache::MetaType::HTile) {
            static constexpr u32 ZmaskUncompressed = 0xf;
            runtime.FillBuffer(arena, arena->Offset(device_addr), size, ZmaskUncompressed);
            return true;
        } else if (meta->type == TextureCache::MetaType::CMask) {
            const auto expanded_value = CmaskColorExpandedValue(meta->num_samples);
            if (!expanded_value) {
                LOG_WARNING(Render_Vulkan,
                            "Unsupported CMask sample count: address={:#x}, size={:#x}, "
                            "samples={}, owner={}:{}",
                            device_addr, size, meta->num_samples, meta->owner_id.index,
                            meta->owner_uid);
                return false;
            }
            runtime.FillBuffer(arena, arena->Offset(device_addr), size, *expanded_value);
            return true;
        } else {
            LOG_WARNING(Render_Vulkan, "Unhandled metadata type {}",
                        magic_enum::enum_name(meta->type));
        }
    }
    const ImageId image_id = texture_cache.FindImageFromRange(device_addr, size);
    if (!image_id) {
        return false;
    }
    Image& image = texture_cache.GetImage(image_id);
    ASSERT_MSG(device_addr == image.info.guest_address,
               "Texel buffer aliases image subresources {:x} : {:x}", device_addr,
               image.info.guest_address);
    const u64 arena_offset = arena->Offset(device_addr);
    boost::container::small_vector<vk::BufferImageCopy, 8> buffer_copies;
    for (u32 mip = 0; mip < image.info.resources.levels; mip++) {
        const auto& mip_info = image.info.mips_layout[mip];
        const u32 width = std::max(image.info.size.width >> mip, 1u);
        const u32 height = std::max(image.info.size.height >> mip, 1u);
        const u32 depth = std::max(image.info.size.depth >> mip, 1u);
        if (arena_offset + mip_info.offset + mip_info.size > arena->size_bytes) {
            break;
        }
        buffer_copies.push_back(vk::BufferImageCopy{
            .bufferOffset = mip_info.offset,
            .bufferRowLength = mip_info.pitch,
            .bufferImageHeight = mip_info.height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = mip,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {width, height, depth},
        });
    }
    if (buffer_copies.empty()) {
        return false;
    }
    auto& tile_manager = texture_cache.GetTileManager();
    tile_manager.TileImage(image, buffer_copies, arena, arena_offset);
    return true;
}

void BufferCache::SubmitPendingArenaBinds(Vulkan::SubmitInfo& info) {
    if (pending_binds.empty()) {
        return;
    }

    std::vector<vk::SparseBufferMemoryBindInfo> buffer_binds;
    buffer_binds.reserve(pending_binds.size());

    for (const auto& binds : pending_binds) {
        buffer_binds.emplace_back(vk::SparseBufferMemoryBindInfo{
            .buffer = binds.arena->Handle(),
            .bindCount = static_cast<u32>(binds.binds.size()),
            .pBinds = binds.binds.data(),
        });
    }

    const u64 signal_tick = memory_semaphore.NextTick();
    const auto signal_sema = memory_semaphore.Handle();

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .signalSemaphoreValueCount = 1u,
        .pSignalSemaphoreValues = &signal_tick,
    };

    const vk::BindSparseInfo sparse_info = {
        .pNext = &timeline_si,
        .bufferBindCount = static_cast<u32>(buffer_binds.size()),
        .pBufferBinds = buffer_binds.data(),
        .signalSemaphoreCount = 1u,
        .pSignalSemaphores = &signal_sema,
    };

    info.AddWait(signal_sema, signal_tick);
    // The submit callback runs outside submit_mutex, and the draw scheduler's recording thread
    // submits to the same queue (docs/design/command-recording.md). The bind reaches the queue
    // before queued commands of earlier flushes; that is safe while binds only add pages that no
    // queued command uses.
    std::scoped_lock lk{Vulkan::Scheduler::submit_mutex};
    auto submit_result = instance.GetGraphicsQueue().bindSparse(sparse_info);
    ASSERT_MSG(submit_result != vk::Result::eErrorDeviceLost, "Device lost during submit");

    pending_binds.clear();
}

} // namespace VideoCore
