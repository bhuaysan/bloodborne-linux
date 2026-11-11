// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/core/libraries/gnmdriver/gnmdriver.cpp, gnmdriver_init.h
// @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-11: the submit, flip, compute queue and equeue calls the
// game imports, on our runtime. Liverpool through our CommandProcessor (without one the buffers
// are dropped), GPU events into our EventQueue keyed by its handle; a second registration of a
// queue replaces the first, and a source above 0x40 fails with EINVAL instead of asserting. The
// init sequence is InitSequence200 only, the one upstream picks for the eboot's SDK version
// 0x02000071. A compute queue map also checks that the ring and the read pointer are guest memory
// and refuses slots past the compute rings; unmapping or ringing an unknown queue and a ring
// offset past the ring are logged and ignored. A submit-and-flip without buffers fails. The
// submission lock is set under its mutex. No debug-state dumps, workloads or Neo sequences.
// License texts: third_party/shadps4/LICENSES/
//
// The stage registers are in gnm_shader.cpp, the default state, markers and profiling queries in
// gnm_state.cpp; the other imports stay stubs.

#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <span>

#include "common/assert.h"
#include "common/log.hpp"
#include "core/platform.h"
#include "hle/hle.hpp"
#include "hle/kernel/abi.hpp"
#include "hle/kernel/equeue.hpp"
#include "hle/kernel/vmem.hpp"
#include "hle/video/video.hpp"
#include "video_core/amdgpu/pm4_cmds.h"

namespace bb::hle::video {
namespace {

using namespace AmdGpu;
using Platform::InterruptId;

constexpr std::string_view channel = "hle.libSceGnmDriver";

// The first submissions and GPU events are logged at info, the rest at debug; every
// stats_frames-th frame logs the counts at info.
constexpr std::uint64_t logged_submits = 3;
constexpr std::uint64_t stats_frames = 600;

// Liverpool's compute pipes and rings (NumComputePipes, NumComputeRings): a compute queue's slot
// index is below compute_rings.
constexpr std::uint32_t compute_pipes = 7;
constexpr std::uint32_t compute_rings = 56;

// clang-format off
constexpr std::array InitSequence200{
    // A fake preamble to mimic context reset sent by FW
    0xc0001200u, 0u, // IT_CLEAR_STATE

    // Actual init state sequence
    0xc0017600u, 0x216u, 0xffffffffu,
    0xc0017600u, 0x217u, 0xffffffffu,
    0xc0017600u, 0x215u, 0u,
    0xc0016900u, 0x2f9u, 0x2du,
    0xc0016900u, 0x282u, 8u,
    0xc0016900u, 0x280u, 0x80008u,
    0xc0016900u, 0x281u, 0xffff0000u,
    0xc0016900u, 0x204u, 0u,
    0xc0016900u, 0x206u, 0x43fu,
    0xc0016900u, 0x83u,  0xffffu,
    0xc0016900u, 0x317u, 0x10u,
    0xc0016900u, 0x2fau, 0x3f800000u,
    0xc0016900u, 0x2fcu, 0x3f800000u,
    0xc0016900u, 0x2fbu, 0x3f800000u,
    0xc0016900u, 0x2fdu, 0x3f800000u,
    0xc0016900u, 0x202u, 0xcc0010u,
    0xc0016900u, 0x30eu, 0xffffffffu,
    0xc0016900u, 0x30fu, 0xffffffffu,
    0xc0002f00u, 1u,
    0xc0017600u, 7u,     0x1701ffu,
    0xc0017600u, 0x46u,  0x1701fdu,
    0xc0017600u, 0x87u,  0x1701ffu,
    0xc0017600u, 0xc7u,  0x1701fdu,
    0xc0017600u, 0x107u, 0x17u,
    0xc0017600u, 0x147u, 0x1701fdu,
    0xc0017600u, 0x47u,  0x1cu,
    0xc0016900u, 0x1b1u, 2u,
    0xc0016900u, 0x101u, 0u,
    0xc0016900u, 0x100u, 0xffffffffu,
    0xc0016900u, 0x103u, 0u,
    0xc0016900u, 0x284u, 0u,
    0xc0016900u, 0x290u, 0u,
    0xc0016900u, 0x2aeu, 0u,
    0xc0016900u, 0x292u, 0u,
    0xc0016900u, 0x293u, 0x6020000u,
    0xc0016900u, 0x2f8u, 0u,
    0xc0016900u, 0x2deu, 0x1e9u,
    0xc0036900u, 0x295u, 0x100u, 0x100u, 4u,
    0xc0017900u, 0x200u, 0xe0000000u,
};
static_assert(InitSequence200.size() == 0x76 + 2);
// clang-format on

std::atomic<CommandProcessor*> liverpool{nullptr};

// In case if `submitDone` is issued we need to block submissions until GPU idle
std::atomic<bool> submission_lock{false};
std::condition_variable cv_lock{};
std::mutex m_wait_idle{};
std::mutex m_submit_lock{};
// Guarded by m_submit_lock.
bool send_init_packet{true};  // initialize HW state before first game's submit in a frame
std::uint64_t frames_submitted{};
std::array<std::uint32_t, compute_rings> asc_next_offs_dw{};

std::atomic<std::uint64_t> submits{0};
std::atomic<std::uint64_t> gpu_events{0};

void ResetSubmissionLock(InterruptId) {
    std::unique_lock lock{m_wait_idle};
    submission_lock = false;
    cv_lock.notify_all();
}

void WaitGpuIdle() {
    std::unique_lock lock{m_wait_idle};
    cv_lock.wait(lock, [] { return !submission_lock; });
}

// The CommandProcessor, or null after logging that `call` drops its work.
CommandProcessor* gpu(std::string_view call) {
    CommandProcessor* const p = liverpool;
    if (p == nullptr) {
        common::log(common::LogLevel::warn, channel, "{}: no GPU, dropped", call);
    }
    return p;
}

bool IsValidEventType(InterruptId id) {
    return (static_cast<u32>(id) >= static_cast<u32>(InterruptId::Compute0RelMem) &&
            static_cast<u32>(id) <= static_cast<u32>(InterruptId::Compute6RelMem)) ||
           static_cast<u32>(id) == static_cast<u32>(InterruptId::GfxEop);
}

bool guest_readable(const void* address, std::size_t bytes) {
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    return kernel::vmem::mapped(start, start + bytes);
}

// The checks of upstream's submits; 0 or submission_invalid_argument.
s32 CheckSubmit(u32 count, const u64* dcb_gpu_addrs, const u32* dcb_sizes_in_bytes,
                const u64* ccb_gpu_addrs, const u32* ccb_sizes_in_bytes) {
    if (count != 0) {
        if (!dcb_gpu_addrs || !dcb_sizes_in_bytes) {
            common::log(common::LogLevel::error, channel,
                        "dcbGpuAddrs and dcbSizesInBytes must not be NULL");
            return submission_invalid_argument;
        }
        if (ccb_sizes_in_bytes && !ccb_gpu_addrs) {
            common::log(common::LogLevel::error, channel,
                        "ccbGpuAddrs must not be NULL if ccbSizesInBytes is non-NULL");
            return submission_invalid_argument;
        }
    }
    for (u32 i = 0; i < count; i++) {
        if (dcb_sizes_in_bytes[i] == 0) {
            common::log(common::LogLevel::error, channel, "Submitting a null DCB {}", i);
            return submission_invalid_argument;
        }
        if (dcb_sizes_in_bytes[i] > 0x3ffffc) {
            common::log(common::LogLevel::error, channel,
                        "dcbSizesInBytes[{}] ({}) is limited to (2*20)-1 DWORDS", i,
                        dcb_sizes_in_bytes[i]);
            return submission_invalid_argument;
        }
        if (ccb_sizes_in_bytes && ccb_sizes_in_bytes[i] > 0x3ffffc) {
            common::log(common::LogLevel::error, channel,
                        "ccbSizesInBytes[{}] ({}) is limited to (2*20)-1 DWORDS", i,
                        ccb_sizes_in_bytes[i]);
            return submission_invalid_argument;
        }
    }
    return 0;
}

// Under m_submit_lock.
s32 PerformSubmit(u32 count, const u64* dcb_gpu_addrs, const u32* dcb_sizes_in_bytes,
                  const u64* ccb_gpu_addrs, const u32* ccb_sizes_in_bytes) {
    const std::uint64_t n = submits.fetch_add(1, std::memory_order_relaxed) + 1;
    common::log(n <= logged_submits ? common::LogLevel::info : common::LogLevel::debug, channel,
                "submission {}: {} buffer(s), first {} bytes", n, count,
                count > 0 ? dcb_sizes_in_bytes[0] : 0);
    CommandProcessor* const cp = gpu("submit");
    if (cp == nullptr) {
        return 0;
    }
    if (send_init_packet) {
        cp->submit_gfx(InitSequence200, {});
        send_init_packet = false;
    }

    for (auto cbpair = 0u; cbpair < count; ++cbpair) {
        const auto* ccb =
            ccb_gpu_addrs ? reinterpret_cast<const u32*>(ccb_gpu_addrs[cbpair]) : nullptr;
        const auto ccb_size_in_bytes = ccb_sizes_in_bytes ? ccb_sizes_in_bytes[cbpair] : 0;

        const auto dcb_size_dw = dcb_sizes_in_bytes[cbpair] >> 2;
        const auto ccb_size_dw = ccb ? ccb_size_in_bytes >> 2 : 0;

        const auto dcb_span =
            std::span{reinterpret_cast<const u32*>(dcb_gpu_addrs[cbpair]), dcb_size_dw};
        const auto ccb_span = std::span{ccb, ccb_size_dw};
        cp->submit_gfx(dcb_span, ccb_span);
    }
    return 0;
}

s32 PatchFlipRequest(u32* cmdbuf, u32 size, u32 vo_handle, u32 buf_idx, s64 flip_arg) {
    // check for `prepareFlip` packet
    ASSERT_MSG(size >= 64, "Can't find `prepareFlip` packet");
    cmdbuf += size - 64;
    ASSERT_MSG(cmdbuf[0] == 0xc03e1000, "Can't find `prepareFlip` packet");

    std::array<u32, 7> backup{};
    std::memcpy(backup.data(), cmdbuf, backup.size() * sizeof(decltype(backup)::value_type));

    ASSERT_MSG(((backup[2] & 3) == 0u) || (backup[1] != PM4CmdNop::PayloadType::PrepareFlipLabel),
               "Invalid flip packet");
    ASSERT_MSG(buf_idx != 0xffff'ffffu, "Invalid VO buffer index");

    const s32 flip_result = submit_eop_flip(static_cast<s32>(vo_handle), buf_idx, flip_arg);
    if (flip_result != 0) {
        if (flip_result == video_out_flip_queue_full) {
            common::log(common::LogLevel::error, channel, "Flip queue is full");
            return submission_flip_queue_full;
        } else {
            common::log(common::LogLevel::error, channel, "Flip request failed");
            return flip_result;
        }
    }

    uintptr_t label_addr{};
    ASSERT_MSG(get_buffer_label_address(static_cast<s32>(vo_handle), &label_addr) == 16,
               "sceVideoOutGetBufferLabelAddress call failed");

    // Write event to lock the VO surface
    auto* write_lock = reinterpret_cast<PM4CmdWriteData*>(cmdbuf);
    write_lock->header = PM4Type3Header{PM4ItOpcode::WriteData, 3};
    write_lock->raw = 0x500u;
    const auto addr = (label_addr + buf_idx * sizeof(label_addr)) & ~0x3ull;
    write_lock->Address<uintptr_t>(addr);
    write_lock->data[0] = 1;

    auto* nop = reinterpret_cast<PM4CmdNop*>(cmdbuf + 5);

    if (backup[1] == PM4CmdNop::PayloadType::PrepareFlip) {
        nop->header = PM4Type3Header{PM4ItOpcode::Nop, 0x39};
        nop->data_block[0] = PM4CmdNop::PayloadType::PatchedFlip;
    } else {
        if (backup[1] == PM4CmdNop::PayloadType::PrepareFlipLabel) {
            nop->header = PM4Type3Header{PM4ItOpcode::Nop, 0x34};
            nop->data_block[0] = PM4CmdNop::PayloadType::PatchedFlip;

            // Write event to update label
            auto* write_label = reinterpret_cast<PM4CmdWriteData*>(cmdbuf + 0x3b);
            write_label->header = PM4Type3Header{PM4ItOpcode::WriteData, 3};
            write_label->raw = 0x500u;
            write_label->dst_addr_lo = backup[2] & 0xffff'fffcu;
            write_label->dst_addr_hi = backup[3];
            write_label->data[0] = backup[4];
        }
        if (backup[1] == PM4CmdNop::PayloadType::PrepareFlipInterruptLabel) {
            nop->header = PM4Type3Header{PM4ItOpcode::Nop, 0x33};
            nop->data_block[0] = PM4CmdNop::PayloadType::PatchedFlip;

            auto* write_eop = reinterpret_cast<PM4CmdEventWriteEop*>(cmdbuf + 0x3a);
            write_eop->header = PM4Type3Header{PM4ItOpcode::EventWriteEop, 4};
            write_eop->event_control = (backup[5] & 0x3f) + 0x500u + (backup[6] & 0x3f) * 0x1000;
            write_eop->address_lo = backup[2] & 0xffff'fffcu;
            write_eop->data_control = (backup[3] & 0xffffu) | 0x2200'0000u;
            write_eop->data_lo = backup[4];
            write_eop->data_hi = 0u;
        }
        if (backup[1] == PM4CmdNop::PayloadType::PrepareFlipInterrupt) {
            nop->header = PM4Type3Header{PM4ItOpcode::Nop, 0x33};
            nop->data_block[0] = PM4CmdNop::PayloadType::PatchedFlip;

            auto* write_eop = reinterpret_cast<PM4CmdEventWriteEop*>(cmdbuf + 0x3a);
            write_eop->header = PM4Type3Header{PM4ItOpcode::EventWriteEop, 4};
            write_eop->event_control = (backup[5] & 0x3f) + 0x500u + (backup[6] & 0x3f) * 0x1000;
            write_eop->address_lo = 0u;
            write_eop->data_control = 0x100'0000u;
            write_eop->data_lo = 0u;
            write_eop->data_hi = 0u;
        }
    }

    return 0;
}

// The end of a frame's submissions, under m_submit_lock.
void SubmitDoneLocked() {
    if (CommandProcessor* const cp = liverpool) {
        if (!cp->is_gpu_idle()) {
            const std::scoped_lock lock{m_wait_idle};
            submission_lock = true;
        }
        cp->submit_done();
    }
    send_init_packet = true;
    if (++frames_submitted % stats_frames == 0) {
        common::log(common::LogLevel::info, channel, "frame {}: {} submissions, {} GPU events",
                    frames_submitted, submits.load(), gpu_events.load());
    }
}

}  // namespace

void start_gnm(CommandProcessor* gpu) {
    static const bool started = [] {
        auto* const controller = Platform::IrqC::Instance();
        // Each interrupt's context exists before the GPU thread signals one: the controller's map
        // of them takes no lock of its own.
        for (u32 id = 0; id <= static_cast<u32>(InterruptId::InterruptIdMax); ++id) {
            controller->Unregister(static_cast<InterruptId>(id), nullptr);
        }
        controller->Register(InterruptId::GpuIdle, ResetSubmissionLock, nullptr);
        return true;
    }();
    (void)started;
    liverpool = gpu;
}

std::int32_t gnm_submit(std::uint32_t count, const std::uint64_t* draw,
                        const std::uint32_t* draw_sizes, const std::uint64_t* ccb,
                        const std::uint32_t* ccb_sizes) {
    if (draw == nullptr || draw_sizes == nullptr) {
        common::log(common::LogLevel::error, channel,
                    "dcbGpuAddrs and dcbSizesInBytes must not be NULL");
        return submission_invalid_argument;
    }
    if (const s32 result = CheckSubmit(count, draw, draw_sizes, ccb, ccb_sizes); result != 0) {
        return result;
    }
    std::scoped_lock lk{m_submit_lock};
    WaitGpuIdle();
    return PerformSubmit(count, draw, draw_sizes, ccb, ccb_sizes);
}

std::int32_t gnm_submit_other(std::uint32_t count, const std::uint64_t*,
                              const std::uint32_t* draw_sizes, const std::uint64_t*,
                              const std::uint32_t*) {
    common::log(common::LogLevel::debug, channel, "validate: {} buffer(s), first {} bytes",
                count, count > 0 && draw_sizes != nullptr ? draw_sizes[0] : 0);
    return validation_not_enabled;
}

std::int32_t gnm_submit_and_flip(std::uint32_t count, const std::uint64_t* draw,
                                 const std::uint32_t* draw_sizes, const std::uint64_t* ccb,
                                 const std::uint32_t* ccb_sizes, std::int32_t handle,
                                 std::int32_t index, std::int32_t, std::int64_t arg) {
    if (count == 0) {
        common::log(common::LogLevel::error, channel, "submit and flip without a DCB");
        return submission_invalid_argument;
    }
    if (const s32 result = CheckSubmit(count, draw, draw_sizes, ccb, ccb_sizes); result != 0) {
        return result;
    }
    auto* cmdbuf = reinterpret_cast<u32*>(draw[count - 1]);
    const auto size_dw = draw_sizes[count - 1] / 4;

    std::scoped_lock lk{m_submit_lock};
    WaitGpuIdle();

    const s32 patch_result = PatchFlipRequest(cmdbuf, size_dw, static_cast<u32>(handle),
                                              static_cast<u32>(index), arg);
    if (patch_result != 0) {
        return patch_result;
    }
    return PerformSubmit(count, draw, draw_sizes, ccb, ccb_sizes);
}

std::int32_t gnm_request_flip_and_submit_done(std::uint32_t* gpu_addr,
                                              std::uint32_t gpu_addr_size_in_bytes,
                                              std::int32_t vo_handle, std::uint32_t buf_idx,
                                              std::uint32_t, std::int64_t flip_arg) {
    if (gpu_addr_size_in_bytes < 0x100) {
        return submission_not_enough_resources;
    }

    std::scoped_lock lk{m_submit_lock};
    WaitGpuIdle();

    // Write a PrepareFlip, then patch the flip request
    auto* nop = reinterpret_cast<PM4CmdNop*>(gpu_addr);
    nop->header = PM4Type3Header{PM4ItOpcode::Nop, 0x3e};
    nop->data_block[0] = PM4CmdNop::PayloadType::PrepareFlip;
    const s32 patch_result =
        PatchFlipRequest(gpu_addr, 0x40, static_cast<u32>(vo_handle), buf_idx, flip_arg);
    if (patch_result != 0) {
        return patch_result;
    }

    // Submit
    if (CommandProcessor* const cp = gpu("request flip")) {
        cp->submit_gfx(std::span<const u32>{gpu_addr, 0x40}, {});
    }
    SubmitDoneLocked();
    return 0;
}

std::int32_t gnm_map_compute_queue(std::uint32_t pipe_id, std::uint32_t queue_id,
                                   const void* ring_base_addr, std::uint32_t ring_size_dw,
                                   std::uint32_t* read_ptr_addr) {
    const auto ring = reinterpret_cast<std::uintptr_t>(ring_base_addr);
    std::int32_t result = 0;
    CommandProcessor* const cp = liverpool;
    if (pipe_id >= compute_pipes) {
        result = compute_queue_invalid_pipe;
    } else if (queue_id >= 8) {  // Liverpool::NumQueuesPerPipe
        result = compute_queue_invalid_queue;
    } else if (ring % 256 != 0) {  // alignment check
        result = compute_queue_invalid_ring;
    } else if (!std::has_single_bit(ring_size_dw)) {
        result = compute_queue_invalid_size;
    } else if (!guest_readable(ring_base_addr, std::size_t{ring_size_dw} * 4)) {
        result = compute_queue_invalid_ring;
    } else if (reinterpret_cast<std::uintptr_t>(read_ptr_addr) % 4 != 0 ||  // alignment check
               !guest_readable(read_ptr_addr, 4)) {
        result = compute_queue_invalid_read_pointer;
    } else if (cp == nullptr) {
        result = compute_queue_internal;
    } else {
        const std::uint32_t vqid = cp->map_queue(reinterpret_cast<std::uint32_t*>(ring),
                                                 ring_size_dw, read_ptr_addr, pipe_id);
        if (vqid >= compute_rings) {
            cp->unmap_queue(vqid);
            result = compute_queue_internal;
        } else {
            // We need to offset index as `dingDong` assumes it to be from the range [1..64]
            result = static_cast<std::int32_t>(vqid + 1);
            *read_ptr_addr = 0u;
            const std::scoped_lock lk{m_submit_lock};
            asc_next_offs_dw[vqid] = 0;
        }
    }
    common::log(result > 0 ? common::LogLevel::info : common::LogLevel::warn, channel,
                "ASC pipe {} queue {} (ring {}, {:#x} dwords) mapped to vqueue {:#x}", pipe_id,
                queue_id, ring_base_addr, ring_size_dw, static_cast<std::uint32_t>(result));
    return result;
}

std::int32_t gnm_map_compute_queue_with_priority(std::uint32_t pipe, std::uint32_t queue,
                                                 const void* ring, std::uint32_t ring_dwords,
                                                 std::uint32_t* read_pointer, std::uint32_t) {
    return gnm_map_compute_queue(pipe, queue, ring, ring_dwords, read_pointer);
}

std::int32_t gnm_unmap_compute_queue(std::uint32_t vqid) {
    CommandProcessor* const cp = liverpool;
    if (vqid == 0 || cp == nullptr || !cp->unmap_queue(vqid - 1)) {
        common::log(common::LogLevel::warn, channel, "unmap of unknown compute queue {}", vqid);
    }
    return 0;
}

void gnm_ding_dong(std::uint32_t gnm_vqid, std::uint32_t next_offs_dw) {
    common::log(common::LogLevel::trace, channel, "vqid {}, offset_dw {}", gnm_vqid,
                next_offs_dw);
    if (gnm_vqid == 0) {
        return;
    }

    std::scoped_lock lk{m_submit_lock};
    WaitGpuIdle();

    const auto vqid = gnm_vqid - 1;
    CommandProcessor* const cp = liverpool;
    const std::span<const u32> ring =
        cp != nullptr && vqid < compute_rings ? cp->queue_ring(vqid) : std::span<const u32>{};
    if (ring.empty() || next_offs_dw > ring.size()) {
        common::log(common::LogLevel::warn, channel,
                    "doorbell of compute queue {} at {:#x} dwords ignored", gnm_vqid,
                    next_offs_dw);
        return;
    }

    auto& offs_dw = asc_next_offs_dw[vqid];

    if (next_offs_dw == offs_dw) {
        return;
    }

    if (next_offs_dw < offs_dw && next_offs_dw != 0) {
        // For cases if a submission is split at the end of the ring buffer, we need to submit it in
        // two parts to handle the wrap
        cp->submit_asc(gnm_vqid, ring.subspan(offs_dw));
        offs_dw = 0;
    }

    const auto acb_size_dw = (next_offs_dw ? next_offs_dw : ring.size()) - offs_dw;
    const std::span acb_span = ring.subspan(offs_dw, acb_size_dw);

    asc_next_offs_dw[vqid] = next_offs_dw;
    cp->submit_asc(gnm_vqid, acb_span);
}

std::int32_t sceGnmSubmitDone() {
    std::scoped_lock lk{m_submit_lock};
    WaitGpuIdle();
    SubmitDoneLocked();
    return 0;
}

std::int32_t sceGnmAreSubmitsAllowed() {
    return submission_lock ? 0 : 1;
}

std::int32_t sceGnmAddEqEvent(std::uint64_t eq, std::uint64_t id, std::uint64_t udata) {
    if (!EventQueue::find(eq)) {
        return kernel::sce_fail(EBADF);
    }
    if (id > static_cast<u32>(InterruptId::InterruptIdMax)) {
        common::log(common::LogLevel::error, channel, "add eq event: interrupt {:#x}", id);
        return kernel::sce_fail(EINVAL);
    }
    common::log(common::LogLevel::info, channel, "add eq event: queue {:#x}, interrupt {:#x}", eq,
                id);
    auto* const controller = Platform::IrqC::Instance();
    void* const uid = reinterpret_cast<void*>(eq);
    controller->Unregister(static_cast<InterruptId>(id), uid);  // a second registration replaces
    controller->Register(
        static_cast<InterruptId>(id),
        [=](InterruptId irq) {
            ASSERT_MSG(irq == static_cast<InterruptId>(id), "An unexpected IRQ occured");

            // We need to convert IRQ# to event id
            if (!IsValidEventType(irq)) {
                return;
            }

            // Event data is expected to be an event type as per sceGnmGetEqEventType.
            EventQueue::trigger(eq, {.id = id,
                                     .kind = gnm_event_kind,
                                     .data = static_cast<std::int64_t>(id),
                                     .user_data = udata});
            const std::uint64_t n = gpu_events.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= logged_submits) {
                common::log(common::LogLevel::info, channel, "gpu event {} (source {:#x})", n,
                            id);
            }
        },
        uid);
    return 0;
}

std::int32_t sceGnmDeleteEqEvent(std::uint64_t eq, std::uint64_t id) {
    if (!EventQueue::find(eq)) {
        return kernel::sce_fail(EBADF);
    }
    if (id <= static_cast<u32>(InterruptId::InterruptIdMax)) {
        Platform::IrqC::Instance()->Unregister(static_cast<InterruptId>(id),
                                               reinterpret_cast<void*>(eq));
    }
    return 0;
}

std::int32_t sceGnmGetEqEventType(const GuestEvent* event) {
    return event != nullptr ? static_cast<std::int32_t>(event->data) : 0;
}

// sceGnmSubmitCommandBuffers
BB_HLE_FUNCTION("libSceGnmDriver", "zwY0YV91TTI", gnm_submit);
// sceGnmValidateCommandBuffers
BB_HLE_FUNCTION("libSceGnmDriver", "iCO804ZgzdA", gnm_submit_other);
// sceGnmSubmitAndFlipCommandBuffers
BB_HLE_FUNCTION("libSceGnmDriver", "xbxNatawohc", gnm_submit_and_flip);
// sceGnmRequestFlipAndSubmitDone
BB_HLE_FUNCTION("libSceGnmDriver", "gObODli-OH8", gnm_request_flip_and_submit_done);
BB_HLE_FUNCTION("libSceGnmDriver", "29oKvKXzEZo", gnm_map_compute_queue);  // sceGnmMapComputeQueue
// sceGnmMapComputeQueueWithPriority
BB_HLE_FUNCTION("libSceGnmDriver", "A+uGq+3KFtQ", gnm_map_compute_queue_with_priority);
// sceGnmUnmapComputeQueue
BB_HLE_FUNCTION("libSceGnmDriver", "ArSg-TGinhk", gnm_unmap_compute_queue);
BB_HLE_FUNCTION("libSceGnmDriver", "bX5IbRvECXk", gnm_ding_dong);  // sceGnmDingDong
BB_HLE_FUNCTION("libSceGnmDriver", "yvZ73uQUqrk", sceGnmSubmitDone);
BB_HLE_FUNCTION("libSceGnmDriver", "b08AgtPlHPg", sceGnmAreSubmitsAllowed);
BB_HLE_FUNCTION("libSceGnmDriver", "b0xyllnVY-I", sceGnmAddEqEvent);
BB_HLE_FUNCTION("libSceGnmDriver", "PVT+fuoS9gU", sceGnmDeleteEqEvent);
BB_HLE_FUNCTION("libSceGnmDriver", "UoYY0DWMC0U", sceGnmGetEqEventType);

}  // namespace bb::hle::video
