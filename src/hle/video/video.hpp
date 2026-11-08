// Video out and GPU submission (docs/design/m3-core-os.md, "Video out without a GPU").
// Ports open and take buffers, and a submitted flip completes at the next 1/60 s tick: its flip
// count goes up, the flip observer (if any) presents it, and every queue registered for the
// port's flip event gets an event. The GPU submit calls queue their draw command buffers on the
// GPU thread (gpu_thread()), which parses them; nothing is drawn yet.
//
// Addresses in the comments are the eboot call sites whose use fixed each result. Two code paths
// drive video out: 0x11e488e..0x11e4915 (open, flip event, two 1920x1080 buffers) with flips
// from sceVideoOutSubmitFlip at 0x11e419a, 0x11e420a, 0x11e4560 and 0x11e4ae0, each followed by
// a wait on the flip queue without a timeout; and 0x26d61d4..0x26d65f3 with flips from the GPU
// submit-and-flip call (0x26b0dff, 0x26b0ee2) and a wait at 0x26d6ee9..0x26d6f26 until the flip
// count at +0x18 of the flip status has caught up.
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>

#include "gpu/pm4/gpu_thread.hpp"
#include "hle/kernel/equeue.hpp"

namespace bb::hle::video {

// The result of every video out call on a handle that is not open, or with bad arguments. The
// game stores this value as "no port" before it opens one (0x26d619e); the callers check only
// the sign.
inline constexpr std::int32_t video_out_error = static_cast<std::int32_t>(0x80290001u);

// The flip events this module triggers (GuestEvent, docs/formats/kernel-abi.md, "Event queue";
// no caller reads a field): id is the port handle, kind is flip_event_kind, detail the buffer
// index, data the flip argument, user_data the value given at registration.
inline constexpr std::int16_t flip_event_kind = 1;
// The GPU events (sceGnmAddEqEvent): id and data are the interrupt source, kind is
// gnm_event_kind, user_data the value given at registration.
inline constexpr std::int16_t gnm_event_kind = 2;

// The interval of flip completion.
using Tick = std::chrono::duration<std::int64_t, std::ratio<1, 60>>;

// libSceVideoOut

// Opens a port and returns its handle, from 1, distinct for every open. 0x11e488e and 0x26d61d4
// pass zeros and treat a negative result as failure (0x26d61df).
std::int32_t sceVideoOutOpen(std::int32_t user_id, std::int32_t type, std::int32_t index,
                             const void* param);

// Closes a port. Its pending flips are dropped and its registrations end. The six callers
// (0x26d636b..0x26d6a9b) ignore the result.
std::int32_t sceVideoOutClose(std::int32_t handle);

// NID CBiu4mCE1DA, sceVideoOutSetFlipRate. 0x26d61ee calls it as (handle, 0) right after the open
// and fails on a negative result. Returns 0 for an open port.
std::int32_t video_out_configure(std::int32_t handle, std::int32_t value);

// Registers queue `equeue` for the port's flip events, with `user_data` for the events.
// 0x11e48bb and 0x26d6234 pass 0 and fail on a negative result (0x26d6239). A second
// registration of the same queue replaces the first.
std::int32_t sceVideoOutAddFlipEvent(std::uint64_t equeue, std::int32_t handle,
                                     std::uint64_t user_data);

// Fills the buffer attribute block at `attr` that sceVideoOutRegisterBuffers then takes. The
// game never reads it back, so the layout is ours (BufferAttribute). The callers reserve 0x78
// (0x11e48f8) and 0x40 bytes (0x26d6261) and pass 0x80000000, a flag, 0, the width, the height
// and the width again (1920, 1080, 1920 at 0x11e48d4..0x11e48ef).
std::int32_t sceVideoOutSetBufferAttribute(void* attr, std::uint32_t format, std::uint32_t tiling,
                                           std::uint32_t aspect, std::uint32_t width,
                                           std::uint32_t height, std::uint32_t pitch);

// Our layout of the attribute block, 28 bytes.
struct BufferAttribute {
    std::uint32_t format, tiling, aspect, width, height, pitch, reserved;
};

// Records `count` buffer addresses from `addresses` for indices start..start+count-1 of the
// port, with a copy of `*attr`, and returns 0. 0x11e4915 registers 2 from index 0 and ignores
// the result, 0x26d65f3 fails on a negative one. At most max_buffers per port. A count of 0
// succeeds and registers nothing: when its configured buffer count [r14+0x28] is 0, 0x26d626b
// jumps to 0x26d65d6, which still calls with count 0 and fails on a negative result.
std::int32_t sceVideoOutRegisterBuffers(std::int32_t handle, std::int32_t start,
                                        const std::uint64_t* addresses, std::int32_t count,
                                        const void* attr);
inline constexpr std::int32_t max_buffers = 16;

// Queues a flip to buffer `index`. It completes at the first 1/60 s tick after the port's
// previous flip (or after now), so at most one flip completes per tick. Mode and argument are
// logged; the argument goes into the event. Returns 0.
std::int32_t sceVideoOutSubmitFlip(std::int32_t handle, std::int32_t index, std::int32_t mode,
                                   std::int64_t arg);

// What the flip thread reports of each completed flip, before the flip's events go out: the
// port, the buffer index, the buffer's registered address (0 for an index without one) and the
// port's buffer attribute. bb-run presents the flip through it.
using FlipObserver = void (*)(std::int32_t handle, std::int32_t index, std::uint64_t address,
                              const BufferAttribute& attribute);
// Sets the observer, or none with nullptr. Set it before the first flip.
void set_flip_observer(FlipObserver observer);

// Writes the number of completed flips of the port, a uint64, to `status` + 0x18 and returns 0.
// 0x26d6f06 reads only that field; the rest of the block is left alone.
std::int32_t sceVideoOutGetFlipStatus(std::int32_t handle, void* status);
inline constexpr std::size_t flip_count_offset = 0x18;

// libSceGnmDriver. The submits queue each draw command buffer (DCB) that lies in guest memory and
// is whole dwords long as one stream on the graphics queue of gpu_thread(), and each constant
// command buffer (CCB) as one stream on its constant queue; a pair with a buffer that doesn't
// qualify is logged and left out. The submits return before the GPU thread runs the buffers.

// The GPU thread, started on first use. INDIRECT_BUFFER targets are checked against the guest
// mappings.
pm4::GpuThread& gpu_thread();

// Starts the GPU trace (gpu_trace.hpp) into a new file at `path`. Call it before the first use of
// gpu_thread(). False if the file can't be created or a trace is already on.
bool enable_gpu_trace(const std::filesystem::path& path);

// NID zwY0YV91TTI, sceGnmSubmitCommandBuffers. Submits `count` draw command buffers (addresses and
// sizes in bytes) and as many optional constant command buffers (null at 0x26b0c23). Called from
// 12 sites, e.g. 0x1094a02 and 0x26b0c23. Returns 0.
std::int32_t gnm_submit(std::uint32_t count, const std::uint64_t* draw,
                        const std::uint32_t* draw_sizes, const std::uint64_t* ccb,
                        const std::uint32_t* ccb_sizes);

// What sceGnmValidateCommandBuffers returns: validation is not enabled (as on retail firmware).
inline constexpr std::int32_t validation_not_enabled = static_cast<std::int32_t>(0x80d13fffu);

// NID iCO804ZgzdA, sceGnmValidateCommandBuffers. The same arguments as gnm_submit (0x1094c2f,
// 0x26bb94b). Queues nothing and returns validation_not_enabled, which keeps the feature of the
// start-up probe at 0x26bb950 off (docs/formats/gnm.md): its wrapper 0x11fb340 is the validate
// twin of the submit wrapper 0x11fb2c0, so queueing here could run buffers twice.
std::int32_t gnm_submit_other(std::uint32_t count, const std::uint64_t* draw,
                              const std::uint32_t* draw_sizes, const std::uint64_t* ccb,
                              const std::uint32_t* ccb_sizes);

// NID xbxNatawohc, sceGnmSubmitAndFlipCommandBuffers. gnm_submit followed by
// sceVideoOutSubmitFlip(handle, index, mode, arg). It has 8 call sites (0x1094f98, 0x10953a8,
// 0x1096775, 0x109c300, 0x11fb4f8, 0x11fb644, 0x26b0dff, 0x26b0ee2); 0x26b0dd5..0x26b0dff,
// 0x26b0eba..0x26b0ee2 and 0x1094f98 pass the port handle in r9 and the rest on the stack. Returns
// the flip's result. The flip doesn't wait for the GPU thread yet: the presenter shows test images.
std::int32_t gnm_submit_and_flip(std::uint32_t count, const std::uint64_t* draw,
                                 const std::uint32_t* draw_sizes, const std::uint64_t* ccb,
                                 const std::uint32_t* ccb_sizes, std::int32_t handle,
                                 std::int32_t index, std::int32_t mode, std::int64_t arg);

// The errors of sceGnmMapComputeQueue (values from shadPS4's gnm_error.h).
inline constexpr std::int32_t compute_queue_invalid_pipe = static_cast<std::int32_t>(0x80d17000u);
inline constexpr std::int32_t compute_queue_invalid_queue = static_cast<std::int32_t>(0x80d17001u);
inline constexpr std::int32_t compute_queue_invalid_size = static_cast<std::int32_t>(0x80d17002u);
inline constexpr std::int32_t compute_queue_invalid_ring = static_cast<std::int32_t>(0x80d17003u);
inline constexpr std::int32_t compute_queue_invalid_read_pointer =
    static_cast<std::int32_t>(0x80d17004u);
inline constexpr std::int32_t compute_queue_internal = static_cast<std::int32_t>(0x80d170ffu);

// NID 29oKvKXzEZo, sceGnmMapComputeQueue. Maps a compute queue on the game's ring
// (docs/formats/gnm.md, "Compute queues"): `(pipe, queue, ring address, ring size in dwords,
// read-pointer address)`. Its wrapper 0x1089a80 stores a non-negative result as the queue id and
// clears the read pointer; 0x108c7b0 then treats id 0 as a failed map, and the render thread's
// device creation gives up (0x26b1216). Checks as shadPS4 does: pipe below 7, queue below 8, the
// ring 256-byte aligned with a power-of-two size, the read pointer dword aligned; both must lie in
// guest memory. Maps a ring of gpu_thread() (its interrupts report the pipe) and returns its
// index + 1, or one of the errors above.
std::int32_t gnm_map_compute_queue(std::uint32_t pipe, std::uint32_t queue, const void* ring,
                                   std::uint32_t ring_dwords, std::uint32_t* read_pointer);

// NID ArSg-TGinhk, sceGnmUnmapComputeQueue (thunk 0x1089b10, from 0x108c810). Unmaps the queue;
// its queued ring streams are dropped. Returns 0, also for an unknown id (logged).
std::int32_t gnm_unmap_compute_queue(std::uint32_t id);

// NID bX5IbRvECXk, sceGnmDingDong (thunk 0x1089b20, from 0x108c830). The ring of queue `id` is
// written up to `write_offset` dwords (0 after the writer wrapped at the ring's end): the GPU
// thread runs it from the previous offset on and writes the read offset back. An unknown id or an
// offset past the ring is logged and ignored.
void gnm_ding_dong(std::uint32_t id, std::uint32_t write_offset);

// Returns 0 and does nothing else: the GPU thread needs no signal that a frame's submissions are
// complete. Called through 0x1089b50 after the submit-and-flip at 0x26b0dff and 0x26b0ee2.
std::int32_t sceGnmSubmitDone();

// NID b0xyllnVY-I. Registers queue `equeue` for the GPU interrupts of source `id`
// (pm4::gfx_eop_interrupt, 0x40, for the EVENT_WRITE_EOP and RELEASE_MEM interrupts of the
// graphics queue), with `user_data` for the events; a second registration replaces the user data.
// Fails with EBADF for a queue that doesn't exist.
std::int32_t sceGnmAddEqEvent(std::uint64_t equeue, std::uint64_t id, std::uint64_t user_data);

// NID PVT+fuoS9gU. Ends that registration. EBADF for a queue that doesn't exist.
std::int32_t sceGnmDeleteEqEvent(std::uint64_t equeue, std::uint64_t id);

// NID UoYY0DWMC0U. The interrupt source of a GPU event (its data field).
std::int32_t sceGnmGetEqEventType(const GuestEvent* event);

}  // namespace bb::hle::video
