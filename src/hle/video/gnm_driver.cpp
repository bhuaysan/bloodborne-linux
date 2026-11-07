// The GPU submit calls of libSceGnmDriver. The submits queue their draw command buffers on the
// GPU thread (docs/design/graphics.md, "One GPU thread"). The stage registers are in
// gnm_shader.cpp, the default state, markers and profiling queries in gnm_state.cpp; the other
// imports stay stubs.
#include <atomic>
#include <cstdint>
#include <span>
#include <vector>

#include "common/log.hpp"
#include "hle/hle.hpp"
#include "hle/kernel/vmem.hpp"
#include "hle/video/video.hpp"

namespace bb::hle::video {
namespace {

constexpr std::string_view channel = "hle.libSceGnmDriver";

// The first submissions are logged at info, the rest at debug.
constexpr std::uint64_t logged_submits = 3;

std::atomic<std::uint64_t> submits{0};
std::atomic<std::int32_t> compute_queues{0};
std::atomic<bool> logged_ccb{false};

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

// Queues the draw command buffers on the graphics queue. A buffer outside the guest mappings or
// with a size that isn't whole dwords is logged and left out. The constant command buffers wait
// for the constant engine (#272).
void queue_buffers(std::string_view call, std::uint32_t count, const std::uint64_t* draw,
                   const std::uint32_t* draw_sizes, const std::uint64_t* ccb,
                   const std::uint32_t* ccb_sizes) {
    log_submit(call, count, draw_sizes);
    if (draw == nullptr || draw_sizes == nullptr) {
        count = 0;
    }
    std::vector<std::span<const std::uint32_t>> buffers;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto* start = reinterpret_cast<const std::uint32_t*>(draw[i]);
        const std::uint32_t bytes = draw_sizes[i];
        if (bytes == 0) {
            continue;
        }
        if (bytes % 4 != 0 || !guest_readable(start, bytes)) {
            common::log(common::LogLevel::warn, channel,
                        "{}: buffer {} at {:#x}, {:#x} bytes, is not readable guest memory", call,
                        i, draw[i], bytes);
            continue;
        }
        buffers.emplace_back(start, bytes / 4);
        if (ccb != nullptr && ccb_sizes != nullptr && ccb[i] != 0 && ccb_sizes[i] != 0 &&
            !logged_ccb.exchange(true, std::memory_order_relaxed)) {
            common::log(common::LogLevel::warn, channel,
                        "{}: constant command buffers are not run yet", call);
        }
    }
    if (!buffers.empty()) {
        gpu_thread().submit(pm4::Queue::graphics, buffers);
    }
}

}  // namespace

pm4::GpuThread& gpu_thread() {
    static pm4::GpuThread thread(guest_readable);
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

std::int32_t gnm_map_compute_queue(std::uint32_t, std::uint32_t, const void* ring,
                                   std::uint32_t ring_dwords, std::uint32_t*) {
    const std::int32_t id = compute_queues.fetch_add(1, std::memory_order_relaxed) + 1;
    common::log(common::LogLevel::info, channel, "compute queue {}: ring {}, {:#x} dwords", id,
                ring, ring_dwords);
    return id;
}

std::int32_t sceGnmSubmitDone() {
    common::log(common::LogLevel::trace, channel, "submit done");
    return 0;
}

// sceGnmSubmitCommandBuffers
BB_HLE_FUNCTION("libSceGnmDriver", "zwY0YV91TTI", gnm_submit);
// sceGnmValidateCommandBuffers
BB_HLE_FUNCTION("libSceGnmDriver", "iCO804ZgzdA", gnm_submit_other);
// sceGnmSubmitAndFlipCommandBuffers
BB_HLE_FUNCTION("libSceGnmDriver", "xbxNatawohc", gnm_submit_and_flip);
BB_HLE_FUNCTION("libSceGnmDriver", "29oKvKXzEZo", gnm_map_compute_queue);  // sceGnmMapComputeQueue
BB_HLE_FUNCTION("libSceGnmDriver", "yvZ73uQUqrk", sceGnmSubmitDone);

}  // namespace bb::hle::video
