// Per-frame time counters for the CPU side of the GPU thread (docs/test-rig.md, "Profiling").
//
// Parts of the frame are timed with Scope. A Scope reads the CPU's time-stamp counter twice and
// adds to counters of its own thread, all inline, so it costs about 20 ns and the GPU thread
// shares no cache line with the fault handlers of other threads (#594). Once per frame (the
// guest's submit-done) log_frame() collects the threads' counters and writes one line with what
// each part took since the last one. Everything is off unless the log filter enables channel
// "profile" at debug (BB_LOG=profile=debug or `--log profile=debug`): a disabled Scope costs one
// load and a branch. Times are inclusive, so a part inside another counts in both (upload inside
// texture, protect inside fault).
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

#include <x86intrin.h>

namespace bb::common::frame_profile {

enum class Part : unsigned {
    commands,  // the GPU thread running queued command buffers (PM4 processing)
    draw,      // Rasterizer draw and dispatch calls
    pipeline,  // pipeline lookup, with shader translation or a wait for a build inside
    bind,      // binding the resources of one draw or dispatch
    buffer,    // buffer cache: obtaining a buffer and synchronizing it with guest memory
    sync,      // buffer cache: copying CPU-modified guest memory into a buffer (inside buffer)
    resident,  // buffer cache: making arena blocks resident, sparse binds (inside buffer)
    texture,   // texture cache: finding and updating the images of a draw
    render,    // render state and render pass setup of a draw
    vertex,    // vertex and index buffer binding of a draw
    state,     // dynamic state and descriptor writes of a draw or dispatch
    upload,    // refreshing a dirty image from guest memory
    detile,    // detiling an image into its upload buffer (inside upload)
    upload_new,  // the first upload of a cache image object: a new or evicted and recreated texture, not a refresh (inside upload)
    fault,     // write-tracking faults, any thread (the whole handler)
    inval_buffer,   // the buffer cache's share of a fault
    inval_texture,  // the texture cache's share of a fault
    protect,   // mprotect calls of write tracking, from a fault or from a cache sync
    copy,      // GPU-side buffer copies and fills (CopyBuffer, FillBuffer)
    irq,       // interrupt handlers signalled from the GPU thread (end of pipe, flips)
    queue,     // callbacks other threads queued for the GPU thread
    submit,    // end-of-frame housekeeping of the rasterizer
    wait,      // the scheduler waiting for the GPU
    replay,    // replaying recorded Vulkan commands into a command buffer (#676)
    present,   // the presenter thread's present call
    flip_wait, // a flip request waiting from its queueing to the vblank that took it
    count,
};

std::string_view part_name(Part part);

namespace detail {

inline constexpr std::size_t parts = static_cast<std::size_t>(Part::count);

// One thread's counters. They only grow: the owner adds with a plain load and store (no locked
// instruction), take() reads them from another thread and reports what was added since its last
// read. A write-tracking fault is the one signal handler that adds here, and it is synchronous:
// it cannot interrupt an add, which touches no guest memory.
struct alignas(64) ThreadCounters {
    std::array<std::atomic<std::uint64_t>, parts> ticks{};
    std::array<std::atomic<std::uint64_t>, parts> calls{};
};

inline constinit std::atomic<bool> on{false};
[[gnu::tls_model("initial-exec")]] inline constinit thread_local ThreadCounters* mine = nullptr;

// The first add of a thread: claims its counters. Async-signal-safe (no lock, no allocation).
void add_ticks_slow(Part part, std::uint64_t ticks) noexcept;

inline void add_ticks(Part part, std::uint64_t ticks) noexcept {
    ThreadCounters* const c = mine;
    if (c == nullptr) [[unlikely]] {
        add_ticks_slow(part, ticks);
        return;
    }
    const auto i = static_cast<std::size_t>(part);
    c->ticks[i].store(c->ticks[i].load(std::memory_order_relaxed) + ticks,
                      std::memory_order_relaxed);
    c->calls[i].store(c->calls[i].load(std::memory_order_relaxed) + 1,
                      std::memory_order_relaxed);
}

// The time-stamp counter. It has to run at a constant rate on every core (constant_tsc and
// nonstop_tsc, as on every x86-64 CPU since about 2008); take() scales it to ns against
// CLOCK_MONOTONIC.
inline std::uint64_t ticks_now() noexcept {
    return __rdtsc();
}

}  // namespace detail

// Whether Scope records. Follows the log filter (set_log_filter).
inline bool enabled() noexcept {
    return detail::on.load(std::memory_order_relaxed);
}
void set_enabled(bool on) noexcept;

// CLOCK_MONOTONIC in ns, the clock of the pipeline lines and of frames.csv.
std::uint64_t now_ns() noexcept;

// Adds a time measured by the caller and one call. Always counts; for the rare parts that are not
// a Scope (it adds to counters all threads share).
void add(Part part, std::uint64_t ns) noexcept;

class Scope {
public:
    explicit Scope(Part part) noexcept
        : part_(part), start_(enabled() ? detail::ticks_now() : 0) {}
    ~Scope() {
        if (start_ != 0) {
            detail::add_ticks(part_, detail::ticks_now() - start_);
        }
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Part part_;
    std::uint64_t start_;
};

struct Totals {
    std::array<std::uint64_t, static_cast<std::size_t>(Part::count)> ns{};
    std::array<std::uint64_t, static_cast<std::size_t>(Part::count)> calls{};
};

// What was added since the last call, by all threads. One caller at a time gets each addition
// once.
Totals take() noexcept;

// "Frame profile: at <ns> ns, <us> us since the last, <part>=<us> us/<calls> ..." with the parts
// that had a call (tools/rig/frame_profile.py reads it).
std::string format_frame(const Totals& totals, std::uint64_t at_ns, std::uint64_t interval_ns);

// Ends a frame: logs one line (when enabled) with what was added since the last.
void log_frame();

}  // namespace bb::common::frame_profile
