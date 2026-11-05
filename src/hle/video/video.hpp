// Headless video out (docs/design/m3-core-os.md, "Video out without a GPU"). Ports open, and a
// submitted flip completes at the next 1/60 s tick: its flip count goes up and every queue
// registered for the port's flip event gets an event. Nothing is presented (M4).
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

namespace bb::hle::video {

// The result of every video out call on a handle that is not open, or with bad arguments. The
// game stores this value as "no port" before it opens one (0x26d619e); the callers check only
// the sign.
inline constexpr std::int32_t video_out_error = static_cast<std::int32_t>(0x80290001u);

// The flip events this module triggers (GuestEvent, docs/formats/kernel-abi.md, "Event queue";
// no caller reads a field): id is the port handle, kind is flip_event_kind, detail the buffer
// index, data the flip argument, user_data the value given at registration.
inline constexpr std::int16_t flip_event_kind = 1;

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

// Registers queue `equeue` for the port's flip events, with `user_data` for the events.
// 0x11e48bb and 0x26d6234 pass 0 and fail on a negative result (0x26d6239). A second
// registration of the same queue replaces the first.
std::int32_t sceVideoOutAddFlipEvent(std::uint64_t equeue, std::int32_t handle,
                                     std::uint64_t user_data);

// Queues a flip to buffer `index`. It completes at the first 1/60 s tick after the port's
// previous flip (or after now), so at most one flip completes per tick. Mode and argument are
// logged; the argument goes into the event. Returns 0.
std::int32_t sceVideoOutSubmitFlip(std::int32_t handle, std::int32_t index, std::int32_t mode,
                                   std::int64_t arg);

// Writes the number of completed flips of the port, a uint64, to `status` + 0x18 and returns 0.
// 0x26d6f06 reads only that field; the rest of the block is left alone.
std::int32_t sceVideoOutGetFlipStatus(std::int32_t handle, void* status);
inline constexpr std::size_t flip_count_offset = 0x18;

}  // namespace bb::hle::video
