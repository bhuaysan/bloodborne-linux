// The GNM driver's side of Liverpool (docs/design/renderer.md, "GNM and VideoOut glue is
// adapted"): the submit calls of video.hpp hand their command buffers to a CommandProcessor.
// Apart from video.hpp, so that video_core_presenter.hpp needs neither the HLE registry nor the
// kernel.
#pragma once

#include <cstdint>
#include <span>

namespace bb::hle::video {

// What runs the command buffers: video_core's Liverpool (video_core_presenter.hpp), a fake in the
// tests. The calls are the Liverpool calls of upstream's gnmdriver.cpp.
class CommandProcessor {
public:
    virtual ~CommandProcessor() = default;
    // Liverpool::SubmitGfx: a draw command buffer and its constant command buffer (may be empty).
    virtual void submit_gfx(std::span<const std::uint32_t> dcb,
                            std::span<const std::uint32_t> ccb) = 0;
    // Liverpool::SubmitAsc: a part of the ring of compute queue `gnm_vqid` (1..).
    virtual void submit_asc(std::uint32_t gnm_vqid, std::span<const std::uint32_t> acb) = 0;
    // Liverpool::SubmitDone: the frame's submissions are complete.
    virtual void submit_done() = 0;
    // Liverpool::IsGpuIdle.
    virtual bool is_gpu_idle() = 0;
    // asc_queues.Insert: maps a compute queue on `ring` and returns its slot index (gnm_vqid - 1).
    virtual std::uint32_t map_queue(std::uint32_t* ring, std::uint32_t ring_dwords,
                                    std::uint32_t* read_pointer, std::uint32_t pipe) = 0;
    // asc_queues.Erase; false (and nothing erased) for an index that holds no queue.
    virtual bool unmap_queue(std::uint32_t index) = 0;
    // The ring of the queue at `index` (map_addr, ring_size_dw); empty for an index without one.
    virtual std::span<const std::uint32_t> queue_ring(std::uint32_t index) = 0;
};

// The tessellation factor ring: the top 256 MiB of the guest window (vmem.hpp), as upstream's
// SystemReservedVirtualBase + SystemReservedVirtualSize - 0x10000000.
inline constexpr std::uint64_t tessellation_factor_ring_size = 0x10000000;
inline constexpr std::uint64_t tessellation_factor_ring_address =
    0x100'0000'0000 - tessellation_factor_ring_size;  // vmem::window_end - size

// NID ln33zjBrfjk. Reserves the ring in the guest window once and returns its address.
std::uint64_t sceGnmGetTheTessellationFactorRingBufferBaseAddress();

// Starts the GNM driver on `gpu` (nullptr: the submits drop their buffers), as upstream's
// RegisterLib: the submission lock ends at Liverpool's GpuIdle interrupt. bb-run calls it once
// Liverpool exists, before the game starts. A later call replaces `gpu`: the tests switch it while
// nothing is submitted.
void start_gnm(CommandProcessor* gpu);

}  // namespace bb::hle::video
