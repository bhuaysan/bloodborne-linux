// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/core/libraries/videoout/video_out.h, videoout_error.h
// @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-11: the error codes and the flip status of the exports the
// game imports (declared in video.hpp), our FlipPresenter in place of the global presenter and
// Liverpool, start_video_out, and the EOP flip and flip label calls of the GNM driver.
// License texts: third_party/shadps4/LICENSES/
//
// Video out's driver (docs/design/renderer.md, "GNM and VideoOut glue is adapted"): one port, a
// flip queue and a present thread that takes at most one flip per vblank. A completed flip
// updates the flip status, triggers the flip event and clears the flip label of the buffer shown
// before. With a FlipPresenter the flip's frame is prepared on the GPU thread and presented on the
// present thread; without one (--headless) flips complete without presenting.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>

#include "core/libraries/videoout/buffer.h"
#include "core/libraries/videoout/driver.h"

namespace Vulkan {
struct Frame;
}

namespace bb::hle::video {

using Libraries::VideoOut::BufferAttribute;
using Libraries::VideoOut::BufferAttributeGroup;
using Libraries::VideoOut::VideoOutPort;

// The errors (videoout_error.h). The callers check only the sign.
inline constexpr std::int32_t video_out_invalid_value = static_cast<std::int32_t>(0x80290001u);
inline constexpr std::int32_t video_out_invalid_address = static_cast<std::int32_t>(0x80290002u);
inline constexpr std::int32_t video_out_invalid_pitch = static_cast<std::int32_t>(0x80290004u);
inline constexpr std::int32_t video_out_invalid_tiling_mode =
    static_cast<std::int32_t>(0x80290007u);
inline constexpr std::int32_t video_out_invalid_aspect_ratio =
    static_cast<std::int32_t>(0x80290008u);
inline constexpr std::int32_t video_out_resource_busy = static_cast<std::int32_t>(0x80290009u);
inline constexpr std::int32_t video_out_invalid_index = static_cast<std::int32_t>(0x8029000au);
inline constexpr std::int32_t video_out_invalid_handle = static_cast<std::int32_t>(0x8029000bu);
inline constexpr std::int32_t video_out_invalid_event_queue =
    static_cast<std::int32_t>(0x8029000cu);
inline constexpr std::int32_t video_out_no_empty_slot = static_cast<std::int32_t>(0x8029000fu);
inline constexpr std::int32_t video_out_slot_occupied = static_cast<std::int32_t>(0x80290010u);
inline constexpr std::int32_t video_out_flip_queue_full = static_cast<std::int32_t>(0x80290012u);

// The one port's handle, and its number of buffer slots.
inline constexpr std::int32_t main_port_handle = 1;
inline constexpr std::int32_t max_display_buffers = Libraries::VideoOut::MaxDisplayBuffers;

// The highest flip rate: 0, 1 and 2 show a flip at most every first, second or third vblank
// (60, 30 and 20 Hz). Other rates fail.
inline constexpr std::int32_t max_flip_rate = 2;

// The flip event (equeue.h of upstream): ident 6 (OrbisVideoOutInternalEventId::Flip) and filter
// -13 (Filter::VideoOut). Its data holds the flip argument from bit 16 on, a count of 1 in bits
// 12..15 (our queue replaces a pending record instead of counting) and the low 12 bits of the
// time stamp counter. No caller in the game reads a field.
inline constexpr std::uint64_t flip_event_id = 6;
inline constexpr std::int16_t video_out_filter = -13;

// The flip status block (FlipStatus), 64 bytes, written whole by sceVideoOutGetFlipStatus.
struct FlipStatus {
    std::uint64_t count = 0;
    std::uint64_t process_time = 0;  // microseconds, as libkernel's process time
    std::uint64_t tsc = 0;           // libkernel's time stamp counter
    std::int64_t flip_arg = -1;
    std::uint64_t submit_tsc = 0;
    std::uint64_t reserved0 = 0;
    std::int32_t gc_queue_num = 0;
    std::int32_t flip_pending_num = 0;
    std::int32_t current_buffer = -1;
    std::uint32_t reserved1 = 0;
};
static_assert(sizeof(FlipStatus) == 64);

// The vblank period, 60 Hz rounded up to whole nanoseconds.
inline constexpr std::chrono::nanoseconds vblank_period{16'666'667};

// What presents the flips: video_core's presenter on Liverpool's GPU thread in bb-run's window
// (video_core_presenter.hpp), a fake in the tests. The calls are upstream's presenter and
// Liverpool calls of driver.cpp; prepare_* run on the GPU thread, present on the present thread.
class FlipPresenter {
public:
    virtual ~FlipPresenter() = default;
    // Liverpool::SetVoPort: the GPU thread waits on the port's flip labels.
    virtual void set_vo_port(VideoOutPort* port) = 0;
    // Liverpool::SendCommand: runs `work` on the GPU thread, in order with the command buffers.
    virtual void send_command(std::function<void()> work) = 0;
    virtual void register_surface(const BufferAttributeGroup& group, std::uint64_t address) = 0;
    virtual Vulkan::Frame* prepare_frame(const BufferAttributeGroup& group,
                                         std::uint64_t address) = 0;
    virtual Vulkan::Frame* prepare_blank_frame(bool present_thread) = 0;
    virtual Vulkan::Frame* prepare_last_frame() = 0;
    virtual void present(Vulkan::Frame* frame, bool is_reusing_frame, bool is_game_frame) = 0;
    virtual void set_hdr(bool enable) = 0;
    // ImGui::Core::MustKeepDrawing.
    virtual bool must_keep_drawing() = 0;
};

// Starts video out with `presenter` (nullptr: flips complete without presenting) and its present
// thread. bb-run calls it once the presenter exists, before the game starts (docs/design/
// renderer.md, "Startup order"); the first export starts it without a presenter otherwise. A
// later call replaces the presenter: the tests switch it while no flip is pending.
void start_video_out(FlipPresenter* presenter);

// sceVideoOutSubmitEopFlip, for the GNM driver's flip packets: the flip to buffer `index` is
// queued when Liverpool reaches the packet (the GfxFlip interrupt), on its GPU thread, and counts
// in gc_queue_num until it completes. 0, video_out_invalid_handle or video_out_invalid_index.
std::int32_t submit_eop_flip(std::int32_t handle, std::uint32_t index, std::int64_t arg);

// sceVideoOutGetBufferLabelAddress: the address of the port's 16 flip labels, which the flip
// packet sets. Returns 16, or video_out_invalid_address or video_out_invalid_handle.
std::int32_t get_buffer_label_address(std::int32_t handle, std::uintptr_t* address);

}  // namespace bb::hle::video
