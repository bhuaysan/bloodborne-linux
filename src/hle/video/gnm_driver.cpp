// The GPU submit calls of libSceGnmDriver. The submits queue their draw and constant command
// buffers on the GPU thread (docs/design/graphics.md, "One GPU thread"), the compute queue calls map
// the game's rings there and report how far they are written. The stage registers are in
// gnm_shader.cpp, the default state, markers and profiling queries in gnm_state.cpp; the other
// imports stay stubs.
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <iterator>
#include <mutex>
#include <span>
#include <vector>

#include "common/log.hpp"
#include "hle/hle.hpp"
#include "hle/kernel/abi.hpp"
#include "hle/kernel/equeue.hpp"
#include "hle/kernel/vmem.hpp"
#include "hle/video/video.hpp"

namespace bb::hle::video {
namespace {

constexpr std::string_view channel = "hle.libSceGnmDriver";

// The first submissions are logged at info, the rest at debug.
constexpr std::uint64_t logged_submits = 3;

std::atomic<std::uint64_t> submits{0};
// Compute pipes and queues per pipe the map accepts (shadPS4: pipe 7 belongs to the system).
constexpr std::uint32_t compute_pipes = 7;
constexpr std::uint32_t queues_per_pipe = 8;

// The equeue events of sceGnmAddEqEvent: (queue, interrupt source) with the user data.
struct EqEvent {
    std::uint64_t equeue;
    std::uint64_t id;
    std::uint64_t user_data;
};
std::mutex eq_mutex;
std::vector<EqEvent> eq_events;
std::atomic<std::uint64_t> eq_triggered{0};

// On the GPU thread: one event on every queue registered for `source`.
void gpu_interrupt(std::uint32_t source) {
    std::vector<EqEvent> targets;
    {
        const std::scoped_lock lock(eq_mutex);
        std::ranges::copy_if(eq_events, std::back_inserter(targets),
                             [source](const EqEvent& e) { return e.id == source; });
    }
    for (const EqEvent& e : targets) {
        EventQueue::trigger(e.equeue, {.id = e.id,
                                       .kind = gnm_event_kind,
                                       .data = static_cast<std::int64_t>(e.id),
                                       .user_data = e.user_data});
        const std::uint64_t n = eq_triggered.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= logged_submits) {
            common::log(common::LogLevel::info, channel, "gpu event {} (source {:#x})", n, e.id);
        }
    }
}

bool guest_readable(const void* address, std::size_t bytes) {
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    return kernel::vmem::mapped(start, start + bytes);
}

void log_submit(std::string_view call, std::uint32_t count, const std::uint32_t* draw_sizes) {
    const std::uint64_t n = submits.fetch_add(1, std::memory_order_relaxed) + 1;
    const common::LogLevel level =
        n <= logged_submits ? common::LogLevel::info : common::LogLevel::debug;
    common::log(level, channel, "submission {} ({}): {} buffer(s), first {} bytes", n, call,
                count, count > 0 && draw_sizes != nullptr ? draw_sizes[0] : 0);
}

// Whether the buffer at `address` of `bytes` bytes can be queued: whole dwords in guest memory.
// Logs if not.
bool queueable(std::string_view call, std::string_view kind, std::uint32_t i,
               std::uint64_t address, std::uint32_t bytes) {
    if (bytes % 4 == 0 && guest_readable(reinterpret_cast<const void*>(address), bytes)) {
        return true;
    }
    common::log(common::LogLevel::warn, channel,
                "{}: {} {} at {:#x}, {:#x} bytes, is not readable guest memory", call, kind, i,
                address, bytes);
    return false;
}

// Queues the draw command buffers on the graphics queue and the constant command buffers on the
// constant queue. A buffer outside the guest mappings or with a size that isn't whole dwords is
// logged and left out together with the other buffer of its pair, whose counters would otherwise
// wait for packets that never run.
void queue_buffers(std::string_view call, std::uint32_t count, const std::uint64_t* draw,
                   const std::uint32_t* draw_sizes, const std::uint64_t* ccb,
                   const std::uint32_t* ccb_sizes) {
    log_submit(call, count, draw_sizes);
    if (draw == nullptr || draw_sizes == nullptr) {
        count = 0;
    }
    if (ccb == nullptr || ccb_sizes == nullptr) {
        ccb = nullptr;
    }
    std::vector<std::span<const std::uint32_t>> draws;
    std::vector<std::span<const std::uint32_t>> constants;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t draw_bytes = draw_sizes[i];
        const std::uint32_t ccb_bytes = ccb != nullptr && ccb[i] != 0 ? ccb_sizes[i] : 0;
        if ((draw_bytes != 0 && !queueable(call, "buffer", i, draw[i], draw_bytes)) ||
            (ccb_bytes != 0 && !queueable(call, "constant buffer", i, ccb[i], ccb_bytes))) {
            continue;
        }
        if (draw_bytes != 0) {
            draws.emplace_back(reinterpret_cast<const std::uint32_t*>(draw[i]), draw_bytes / 4);
        }
        if (ccb_bytes != 0) {
            constants.emplace_back(reinterpret_cast<const std::uint32_t*>(ccb[i]), ccb_bytes / 4);
        }
    }
    if (!constants.empty()) {
        gpu_thread().submit(pm4::Queue::constant, constants);
    }
    if (!draws.empty()) {
        gpu_thread().submit(pm4::Queue::graphics, draws);
    }
}

}  // namespace

pm4::GpuThread& gpu_thread() {
    static pm4::GpuThread thread(guest_readable, gpu_interrupt);
    return thread;
}

std::int32_t gnm_submit(std::uint32_t count, const std::uint64_t* draw,
                        const std::uint32_t* draw_sizes, const std::uint64_t* ccb,
                        const std::uint32_t* ccb_sizes) {
    queue_buffers("submit", count, draw, draw_sizes, ccb, ccb_sizes);
    return 0;
}

std::int32_t gnm_submit_other(std::uint32_t count, const std::uint64_t*,
                              const std::uint32_t* draw_sizes, const std::uint64_t*,
                              const std::uint32_t*) {
    log_submit("validate", count, draw_sizes);
    return validation_not_enabled;
}

std::int32_t gnm_submit_and_flip(std::uint32_t count, const std::uint64_t* draw,
                                 const std::uint32_t* draw_sizes, const std::uint64_t* ccb,
                                 const std::uint32_t* ccb_sizes, std::int32_t handle,
                                 std::int32_t index, std::int32_t mode, std::int64_t arg) {
    queue_buffers("submit and flip", count, draw, draw_sizes, ccb, ccb_sizes);
    return sceVideoOutSubmitFlip(handle, index, mode, arg);
}

std::int32_t gnm_map_compute_queue(std::uint32_t pipe, std::uint32_t queue, const void* ring,
                                   std::uint32_t ring_dwords, std::uint32_t* read_pointer) {
    const auto ring_address = reinterpret_cast<std::uintptr_t>(ring);
    std::int32_t result = 0;
    if (pipe >= compute_pipes) {
        result = compute_queue_invalid_pipe;
    } else if (queue >= queues_per_pipe) {
        result = compute_queue_invalid_queue;
    } else if (ring_address % 256 != 0) {
        result = compute_queue_invalid_ring;
    } else if (ring_dwords == 0 || (ring_dwords & (ring_dwords - 1)) != 0) {
        result = compute_queue_invalid_size;
    } else if (!guest_readable(ring, std::size_t{ring_dwords} * 4)) {  // needs the size
        result = compute_queue_invalid_ring;
    } else if (reinterpret_cast<std::uintptr_t>(read_pointer) % 4 != 0 ||
               !guest_readable(read_pointer, 4)) {
        result = compute_queue_invalid_read_pointer;
    } else {
        const int index = gpu_thread().map_ring(
            std::span(static_cast<const std::uint32_t*>(ring), ring_dwords), read_pointer, pipe);
        result = index < 0 ? compute_queue_internal : index + 1;
    }
    common::log(result > 0 ? common::LogLevel::info : common::LogLevel::warn, channel,
                "map compute queue (pipe {}, queue {}): ring {}, {:#x} dwords -> {:#x}", pipe,
                queue, ring, ring_dwords, static_cast<std::uint32_t>(result));
    return result;
}

std::int32_t gnm_unmap_compute_queue(std::uint32_t id) {
    if (id == 0 || !gpu_thread().unmap_ring(static_cast<int>(id) - 1)) {
        common::log(common::LogLevel::warn, channel, "unmap of unknown compute queue {}", id);
    }
    return 0;
}

void gnm_ding_dong(std::uint32_t id, std::uint32_t write_offset) {
    if (id == 0 || !gpu_thread().ring_doorbell(static_cast<int>(id) - 1, write_offset)) {
        common::log(common::LogLevel::warn, channel,
                    "doorbell of compute queue {} at {:#x} dwords ignored", id, write_offset);
        return;
    }
    common::log(common::LogLevel::trace, channel, "doorbell of compute queue {} at {:#x} dwords",
                id, write_offset);
}

std::int32_t sceGnmSubmitDone() {
    common::log(common::LogLevel::trace, channel, "submit done");
    return 0;
}

std::int32_t sceGnmAddEqEvent(std::uint64_t equeue, std::uint64_t id, std::uint64_t user_data) {
    if (!EventQueue::find(equeue)) {
        return kernel::sce_fail(EBADF);
    }
    const std::scoped_lock lock(eq_mutex);
    const auto same = std::ranges::find_if(
        eq_events, [&](const EqEvent& e) { return e.equeue == equeue && e.id == id; });
    if (same != eq_events.end()) {
        same->user_data = user_data;
    } else {
        eq_events.push_back({equeue, id, user_data});
    }
    return 0;
}

std::int32_t sceGnmDeleteEqEvent(std::uint64_t equeue, std::uint64_t id) {
    if (!EventQueue::find(equeue)) {
        return kernel::sce_fail(EBADF);
    }
    const std::scoped_lock lock(eq_mutex);
    std::erase_if(eq_events, [&](const EqEvent& e) { return e.equeue == equeue && e.id == id; });
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
BB_HLE_FUNCTION("libSceGnmDriver", "29oKvKXzEZo", gnm_map_compute_queue);  // sceGnmMapComputeQueue
// sceGnmUnmapComputeQueue
BB_HLE_FUNCTION("libSceGnmDriver", "ArSg-TGinhk", gnm_unmap_compute_queue);
BB_HLE_FUNCTION("libSceGnmDriver", "bX5IbRvECXk", gnm_ding_dong);  // sceGnmDingDong
BB_HLE_FUNCTION("libSceGnmDriver", "yvZ73uQUqrk", sceGnmSubmitDone);
BB_HLE_FUNCTION("libSceGnmDriver", "b0xyllnVY-I", sceGnmAddEqEvent);
BB_HLE_FUNCTION("libSceGnmDriver", "PVT+fuoS9gU", sceGnmDeleteEqEvent);
BB_HLE_FUNCTION("libSceGnmDriver", "UoYY0DWMC0U", sceGnmGetEqEventType);

}  // namespace bb::hle::video
