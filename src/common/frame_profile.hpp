// Per-frame time counters for the CPU side of the GPU thread (docs/test-rig.md, "Profiling").
//
// Parts of the frame are timed with Scope and added up in atomics, so a fault handler or a
// second thread may add to them. Once per frame (the guest's submit-done) log_frame() writes
// one line with what each part took since the last one and clears the counters. Everything is
// off unless the log filter enables channel "profile" at debug (BB_LOG=profile=debug or
// `--log profile=debug`): a disabled Scope costs a few ns (one call and branch). Times are inclusive, so a
// part inside another counts in both (upload inside texture, protect inside fault).
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

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
    upload_new,  // the first upload of an image: a new texture, not a refresh (inside upload)
    fault,     // write-tracking faults, any thread (the whole handler)
    inval_buffer,   // the buffer cache's share of a fault
    inval_texture,  // the texture cache's share of a fault
    protect,   // mprotect calls of write tracking, from a fault or from a cache sync
    copy,      // GPU-side buffer copies and fills (CopyBuffer, FillBuffer)
    irq,       // interrupt handlers signalled from the GPU thread (end of pipe, flips)
    queue,     // callbacks other threads queued for the GPU thread
    submit,    // end-of-frame housekeeping of the rasterizer
    wait,      // the scheduler waiting for the GPU
    present,   // the presenter thread's present call
    flip_wait, // a flip request waiting from its queueing to the vblank that took it
    count,
};

std::string_view part_name(Part part);

// Whether Scope and add() record. Follows the log filter (set_log_filter).
bool enabled() noexcept;
void set_enabled(bool on) noexcept;

// CLOCK_MONOTONIC in ns, the clock of the pipeline lines and of frames.csv.
std::uint64_t now_ns() noexcept;

void add(Part part, std::uint64_t ns) noexcept;

class Scope {
public:
    explicit Scope(Part part) noexcept : part_(part), start_(enabled() ? now_ns() : 0) {}
    ~Scope() {
        if (start_ != 0) {
            add(part_, now_ns() - start_);
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

// What was added since the last call, and clears the counters.
Totals take() noexcept;

// "Frame profile: at <ns> ns, <us> us since the last, <part>=<us> us/<calls> ..." with the parts
// that had a call (tools/rig/frame_profile.py reads it).
std::string format_frame(const Totals& totals, std::uint64_t at_ns, std::uint64_t interval_ns);

// Ends a frame: logs one line (when enabled) and clears the counters.
void log_frame();

}  // namespace bb::common::frame_profile
