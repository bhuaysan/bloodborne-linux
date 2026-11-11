// Video out and GPU submission. Video out is shadPS4's driver adapted (video_out.hpp,
// docs/design/renderer.md): one port takes buffers, and a submitted flip completes at the first
// vblank after it was queued: the presenter (if any) presents it, the flip status counts it, and
// every queue registered for the port's flip event gets an event. The GPU submit calls are
// shadPS4's GNM driver adapted (gnm_driver.cpp): they hand the command buffers to Liverpool.
//
// Addresses in the comments are the eboot call sites whose use fixed each result. Two code paths
// drive video out: 0x11e488e..0x11e4915 (open, flip event, two 1920x1080 buffers) with flips
// from sceVideoOutSubmitFlip at 0x11e419a, 0x11e420a, 0x11e4560 and 0x11e4ae0, each followed by
// a wait on the flip queue without a timeout; and 0x26d61d4..0x26d65f3 with flips from the GPU
// submit-and-flip call (0x26b0dff, 0x26b0ee2) and a wait at 0x26d6ee9..0x26d6f26 until the flip
// argument at +0x18 of the flip status has caught up with the game's frame counter.
#pragma once

#include <cstdint>
#include <filesystem>

#include "hle/kernel/equeue.hpp"
#include "hle/video/gnm_driver.hpp"
#include "hle/video/video_out.hpp"

namespace bb::hle::video {

// The GPU events (sceGnmAddEqEvent): id and data are the interrupt source, kind is
// gnm_event_kind (Filter::GraphicsCore of upstream's equeue.h), user_data the value given at
// registration. gfx_eop_event is the source of the graphics queue's end-of-pipe interrupts.
inline constexpr std::int16_t gnm_event_kind = -14;
inline constexpr std::uint64_t gfx_eop_event = 0x40;

// libSceVideoOut. The errors and the flip status are in video_out.hpp; callers check only the
// sign of a result.

// Opens the one port and returns main_port_handle; a second open fails with
// video_out_resource_busy until the port is closed. 0x11e488e and 0x26d61d4 pass zeros (bus type
// 0, index 0, the only ones accepted) and treat a negative result as failure (0x26d61df).
std::int32_t sceVideoOutOpen(std::int32_t user_id, std::int32_t type, std::int32_t index,
                             const void* param);

// Closes the port. Its pending flips are dropped and its buffers, labels, status and
// registrations reset. The six callers (0x26d636b..0x26d6a9b) ignore the result.
std::int32_t sceVideoOutClose(std::int32_t handle);

// NID CBiu4mCE1DA. 0x26d61ee calls it as (handle, 0) right after the open and fails on a negative
// result. The present thread takes a flip at every (rate + 1)-th vblank.
std::int32_t sceVideoOutSetFlipRate(std::int32_t handle, std::int32_t rate);

// Registers queue `equeue` for the port's flip events, with `user_data` for the events.
// 0x11e48bb and 0x26d6234 pass 0 and fail on a negative result (0x26d6239). A second
// registration of the same queue replaces the first.
std::int32_t sceVideoOutAddFlipEvent(std::uint64_t equeue, std::int32_t handle,
                                     std::uint64_t user_data);

// Fills the 40-byte BufferAttribute at `attr` (need not be aligned) that
// sceVideoOutRegisterBuffers then takes. The callers reserve 0x78
// (0x11e48f8) and 0x40 bytes (0x26d6261) and pass 0x80000000, a flag, 0, the width, the height
// and the width again (1920, 1080, 1920 at 0x11e48d4..0x11e48ef).
std::int32_t sceVideoOutSetBufferAttribute(void* attr, std::uint32_t format, std::uint32_t tiling,
                                           std::uint32_t aspect, std::uint32_t width,
                                           std::uint32_t height, std::uint32_t pitch);

// Registers `count` buffer addresses from `addresses` at slots start..start+count-1 of the
// port in a new attribute group, and returns the group's index (0..3). 0x11e4915 registers 2
// from index 0 and ignores the result, 0x26d65f3 fails on a negative one. A count of 0 takes a
// group and registers nothing: when its configured buffer count [r14+0x28] is 0, 0x26d626b
// jumps to 0x26d65d6, which still calls with count 0 and fails on a negative result.
std::int32_t sceVideoOutRegisterBuffers(std::int32_t handle, std::int32_t start,
                                        const std::uint64_t* addresses, std::int32_t count,
                                        const void* attr);

// Queues a flip to buffer `index` (-1: a blank frame). It completes at the first vblank after it
// was queued, at most one per vblank and at most 16 pending. Returns 0.
std::int32_t sceVideoOutSubmitFlip(std::int32_t handle, std::int32_t index, std::int32_t mode,
                                   std::int64_t arg);

// Copies the port's 64-byte FlipStatus to `status` and returns 0. 0x26d6f06 reads flip_arg.
std::int32_t sceVideoOutGetFlipStatus(std::int32_t handle, void* status);

// libSceGnmDriver: upstream's submit calls (gnm_driver.cpp, docs/design/renderer.md, "GNM and
// VideoOut glue is adapted"). They hand the command buffers to the CommandProcessor given to
// start_gnm (gnm_driver.hpp) and return before it runs them; without one they are logged and
// dropped.

// The errors of the submits (gnm_error.h).
inline constexpr std::int32_t submission_invalid_argument = static_cast<std::int32_t>(0x80d11000u);
inline constexpr std::int32_t submission_not_enough_resources =
    static_cast<std::int32_t>(0x80d11001u);
inline constexpr std::int32_t submission_flip_queue_full = static_cast<std::int32_t>(0x80d11081u);

// NID zwY0YV91TTI, sceGnmSubmitCommandBuffers. Submits `count` draw command buffers (addresses and
// sizes in bytes) and as many optional constant command buffers (null at 0x26b0c23). Called from
// 12 sites, e.g. 0x1094a02 and 0x26b0c23. Waits while the submission lock is held, sends upstream's
// init sequence before the first buffer of a frame, and returns 0, or
// submission_invalid_argument for null arrays, an empty draw buffer or one above 0x3ffffc bytes.
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

// NID xbxNatawohc, sceGnmSubmitAndFlipCommandBuffers. It has 8 call sites (0x1094f98, 0x10953a8,
// 0x1096775, 0x109c300, 0x11fb4f8, 0x11fb644, 0x26b0dff, 0x26b0ee2); 0x26b0dd5..0x26b0dff,
// 0x26b0eba..0x26b0ee2 and 0x1094f98 pass the port handle in r9 and the rest on the stack. Checks
// as gnm_submit, then turns the flip packet in the last 64 dwords of the last draw buffer into a
// write of the buffer's flip label and a PatchedFlip packet (upstream's PatchFlipRequest), at
// which the GPU queues the flip with sceVideoOutSubmitFlip(handle, index, mode, arg)'s effect.
// Returns 0 or the error of the checks.
std::int32_t gnm_submit_and_flip(std::uint32_t count, const std::uint64_t* draw,
                                 const std::uint32_t* draw_sizes, const std::uint64_t* ccb,
                                 const std::uint32_t* ccb_sizes, std::int32_t handle,
                                 std::int32_t index, std::int32_t mode, std::int64_t arg);

// NID gObODli-OH8, sceGnmRequestFlipAndSubmitDone (upstream's ...ForWorkload). Writes a flip
// packet of 64 dwords at `cmd`, patches it as gnm_submit_and_flip does, submits it and ends the
// frame as sceGnmSubmitDone. submission_not_enough_resources for less than 0x100 bytes.
std::int32_t gnm_request_flip_and_submit_done(std::uint32_t* cmd, std::uint32_t bytes,
                                              std::int32_t handle, std::uint32_t index,
                                              std::uint32_t mode, std::int64_t arg);

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
// ring 256-byte aligned with a power-of-two size, the read pointer dword aligned; and, unlike
// upstream, that both lie in guest memory. Maps a queue of the CommandProcessor, clears the read
// pointer and returns the queue's slot index + 1, or one of the errors above
// (compute_queue_internal without a CommandProcessor).
std::int32_t gnm_map_compute_queue(std::uint32_t pipe, std::uint32_t queue, const void* ring,
                                   std::uint32_t ring_dwords, std::uint32_t* read_pointer);

// NID A+uGq+3KFtQ, sceGnmMapComputeQueueWithPriority: gnm_map_compute_queue; the priority is
// ignored, as upstream does.
std::int32_t gnm_map_compute_queue_with_priority(std::uint32_t pipe, std::uint32_t queue,
                                                 const void* ring, std::uint32_t ring_dwords,
                                                 std::uint32_t* read_pointer,
                                                 std::uint32_t priority);

// NID ArSg-TGinhk, sceGnmUnmapComputeQueue (thunk 0x1089b10, from 0x108c810). Unmaps the queue.
// Returns 0, also for an unknown id (logged; upstream would erase a free slot).
std::int32_t gnm_unmap_compute_queue(std::uint32_t id);

// NID bX5IbRvECXk, sceGnmDingDong (thunk 0x1089b20, from 0x108c830). The ring of queue `id` is
// written up to `write_offset` dwords (0 after the writer wrapped at the ring's end): submits the
// ring from the previous offset on, in two parts when it wrapped. Waits while the submission lock
// is held. An unknown id or an offset past the ring is logged and ignored.
void gnm_ding_dong(std::uint32_t id, std::uint32_t write_offset);

// NID yvZ73uQUqrk. Ends the frame's submissions: when the GPU is still busy, takes the submission
// lock, which the next submit waits on until the GPU has run everything. The next submit sends
// the init sequence again. Called through 0x1089b50 after the submit-and-flip at 0x26b0dff and
// 0x26b0ee2. Returns 0.
std::int32_t sceGnmSubmitDone();

// NID b08AgtPlHPg. 1 when the submission lock is free, 0 while it is held.
std::int32_t sceGnmAreSubmitsAllowed();

// NID b0xyllnVY-I. Registers queue `equeue` for the GPU interrupts of source `id` (gfx_eop_event
// for the EVENT_WRITE_EOP and RELEASE_MEM interrupts of the graphics queue, 0..6 for the compute
// pipes), with `user_data` for the events; a second registration replaces the user data. Fails
// with EBADF for a queue that doesn't exist. Other sources are accepted and trigger nothing, as
// upstream.
std::int32_t sceGnmAddEqEvent(std::uint64_t equeue, std::uint64_t id, std::uint64_t user_data);

// NID PVT+fuoS9gU. Ends that registration. EBADF for a queue that doesn't exist.
std::int32_t sceGnmDeleteEqEvent(std::uint64_t equeue, std::uint64_t id);

// NID UoYY0DWMC0U. The interrupt source of a GPU event (its data field).
std::int32_t sceGnmGetEqEventType(const GuestEvent* event);

}  // namespace bb::hle::video
