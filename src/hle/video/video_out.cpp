// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/core/libraries/videoout/driver.cpp, driver.h, video_out.cpp
// @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-11: the driver and the eight exports the game imports on
// our runtime. Flip events go to our EventQueue, the presenter and Liverpool calls through
// FlipPresenter (none when headless), times from our libkernel. Vblanks on a fixed 60 Hz grid with
// sleep_until instead of AccurateTimer, and a flip waits for the first vblank after it was queued.
// A flip still in flight when the port closes is presented but leaves the port alone. Negative
// indices and counts, flip rates other than 0..2 and an unknown handle fail instead of being used.
// Surfaces are registered outside the port lock. An EOP flip to a slot outside the port fails. No
// vblank events, resolution status or HDR output modes: the game imports none of them.
// License texts: third_party/shadps4/LICENSES/

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "common/assert.h"
#include "common/log.hpp"
#include "core/platform.h"
#include "hle/hle.hpp"
#include "hle/kernel/equeue.hpp"
#include "hle/kernel/time.hpp"
#include "hle/video/video.hpp"

namespace bb::hle::video {
namespace {

namespace VO = Libraries::VideoOut;
using Clock = std::chrono::steady_clock;

constexpr std::string_view channel = "hle.libSceVideoOut";

// The first flips are logged at info, the rest at debug.
constexpr std::uint64_t logged_flips = 3;

struct Registration {
    std::uint64_t equeue = 0;
    std::uint64_t user_data = 0;
};

// The rest of upstream's VideoOutPort; the labels Liverpool waits on are in the base.
struct Port : VideoOutPort {
    Port() { buffer_labels.fill(0); }

    // Guarded by port_mutex. A slot without a buffer has group index -1.
    std::array<VO::VideoOutBuffer, VO::MaxDisplayBuffers> buffer_slots{};
    std::array<BufferAttributeGroup, VO::MaxDisplayBufferGroups> groups{};
    FlipStatus flip_status;
    std::vector<Registration> flip_events;
    int prev_index = -1;
    // Counts the closes: a flip submitted before the last close no longer touches the port.
    std::uint64_t generation = 0;
    std::mutex port_mutex;

    std::atomic<int> flip_rate{0};
    std::atomic<bool> is_open{false};
    bool is_hdr = false;
};

class Driver {
public:
    Driver() { std::thread([this] { present_thread(); }).detach(); }

    void set_presenter(FlipPresenter* presenter) { presenter_ = presenter; }

    Port* get_port(std::int32_t handle) {
        return handle == main_port_handle ? &port_ : nullptr;
    }

    std::int32_t open() {
        if (port_.is_open.exchange(true)) {
            return video_out_resource_busy;
        }
        if (FlipPresenter* const presenter = presenter_) {
            presenter->set_vo_port(&port_);
        }
        return main_port_handle;
    }

    // Queued flips stay queued: the present thread presents them, which gives their frames back to
    // the presenter, and drops them by their generation.
    void close() {
        const std::scoped_lock lock{port_.port_mutex};
        ++port_.generation;
        port_.is_open = false;
        port_.flip_rate = 0;
        port_.prev_index = -1;
        {
            const std::scoped_lock labels{port_.vo_mutex};
            port_.buffer_labels.fill(0);
        }
        port_.groups = {};
        port_.flip_status = FlipStatus{};
        port_.buffer_slots = {};
        port_.flip_events.clear();
    }

    void add_flip_event(std::uint64_t equeue, std::uint64_t user_data) {
        const std::scoped_lock lock{port_.port_mutex};
        auto& events = port_.flip_events;
        const auto same = std::ranges::find(events, equeue, &Registration::equeue);
        if (same != events.end()) {
            same->user_data = user_data;
        } else {
            events.push_back({equeue, user_data});
        }
    }

    std::int32_t register_buffers(std::int32_t start, const std::uint64_t* addresses,
                                  std::int32_t count, const BufferAttribute& attribute) {
        Port* const port = &port_;
        std::unique_lock lock{port->port_mutex};
        const s32 group_index = static_cast<s32>(
            std::ranges::find(port->groups, false, &BufferAttributeGroup::is_occupied) -
            port->groups.begin());
        if (group_index >= static_cast<s32>(VO::MaxDisplayBufferGroups)) {
            return video_out_no_empty_slot;
        }
        constexpr s32 max_buffers = VO::MaxDisplayBuffers;
        if (start < 0 || count < 0 || start > max_buffers || count > max_buffers ||
            start + count > max_buffers) {
            common::log(common::LogLevel::error, channel,
                        "too many buffers: startIndex = {}, bufferNum = {}", start, count);
            return video_out_invalid_value;
        }
        const s32 end_index = start + count;
        if (std::any_of(port->buffer_slots.begin() + start, port->buffer_slots.begin() + end_index,
                        [](auto& buffer) { return buffer.group_index != -1; })) {
            return video_out_slot_occupied;
        }
        if (attribute.reserved0 != 0 || attribute.reserved1 != 0) {
            common::log(common::LogLevel::error, channel, "invalid reserved members");
            return video_out_invalid_value;
        }
        if (attribute.aspect_ratio != 0) {
            common::log(common::LogLevel::error, channel, "invalid aspect ratio {}",
                        attribute.aspect_ratio);
            return video_out_invalid_aspect_ratio;
        }
        if (attribute.width > attribute.pitch_in_pixel) {
            common::log(common::LogLevel::error, channel, "width {} is larger than pitch {}",
                        attribute.width, attribute.pitch_in_pixel);
            return video_out_invalid_pitch;
        }
        if (attribute.tiling_mode < VO::TilingMode::Tile ||
            attribute.tiling_mode > VO::TilingMode::Linear) {
            common::log(common::LogLevel::error, channel, "invalid tiling mode {}",
                        static_cast<s32>(attribute.tiling_mode));
            return video_out_invalid_tiling_mode;
        }
        common::log(common::LogLevel::info, channel,
                    "startIndex = {}, bufferNum = {}, pixelFormat = {:#x}, tilingMode = {}, "
                    "width = {}, height = {}, pitchInPixel = {}, option = {:#x}",
                    start, count, static_cast<std::uint32_t>(attribute.pixel_format),
                    static_cast<s32>(attribute.tiling_mode), attribute.width, attribute.height,
                    attribute.pitch_in_pixel, attribute.option);

        auto& group = port->groups[group_index];
        group.attrib = attribute;
        group.is_occupied = true;
        const BufferAttributeGroup registered = group;
        for (s32 i = 0; i < count; i++) {
            const std::uint64_t address = addresses[i];
            port->buffer_slots[start + i] = VO::VideoOutBuffer{
                .group_index = group_index,
                .address_left = address,
                .address_right = 0,
            };
            // Reset the flip label also when registering the buffer.
            {
                const std::scoped_lock labels{port->vo_mutex};
                port->buffer_labels[start + i] = 0;
            }
            port->SignalVoLabel();
            common::log(common::LogLevel::info, channel, "buffers[{}] = {:#x}", start + i,
                        address);
        }
        lock.unlock();
        // The presenter's work stays out of the port lock, as upstream holds none here.
        if (FlipPresenter* const presenter = presenter_) {
            for (s32 i = 0; i < count; i++) {
                presenter->register_surface(registered, addresses[i]);
            }
        }
        return group_index;
    }

    // 0, or the error of the export. An EOP flip comes on the GPU thread.
    std::int32_t submit_flip(std::int32_t index, std::int64_t flip_arg, bool eop = false) {
        Port* const port = &port_;
        std::uint64_t generation = 0;
        {
            const std::scoped_lock lock{port->port_mutex};
            if (index != -1 && port->buffer_slots[index].group_index < 0) {
                common::log(common::LogLevel::error, channel,
                            "slot in bufferIndex = {} is not registered", index);
                return video_out_invalid_index;
            }
            if (index != -1 && port->flip_status.flip_pending_num > 16) {
                common::log(common::LogLevel::error, channel, "flip queue is full");
                return video_out_flip_queue_full;
            }
            if (eop) {
                ++port->flip_status.gc_queue_num;
            }
            ++port->flip_status.flip_pending_num;
            port->flip_status.submit_tsc = kernel::process_counter_nsec();
            generation = port->generation;
        }
        // Flips can come from any thread, so the GPU thread prepares their frames in order with
        // the command buffers.
        FlipPresenter* const presenter = presenter_;
        if (presenter != nullptr && !eop) {
            presenter->send_command(
                [=, this] { submit_flip_internal(presenter, generation, index, flip_arg, eop); });
        } else {
            submit_flip_internal(presenter, generation, index, flip_arg, eop);
        }
        return 0;
    }

    void get_flip_status(void* status) {
        const std::scoped_lock lock{port_.port_mutex};
        std::memcpy(status, &port_.flip_status, sizeof port_.flip_status);
    }

    std::uint64_t count_submission() { return ++submitted_; }

private:
    struct Request {
        Vulkan::Frame* frame;
        FlipPresenter* presenter;
        s64 flip_arg;
        s32 index;
        bool eop;
        std::uint64_t generation;
        Clock::time_point queued;
    };

    void submit_flip_internal(FlipPresenter* presenter, std::uint64_t generation, s32 index,
                              s64 flip_arg, bool eop) {
        BufferAttributeGroup group;
        std::uint64_t address = 0;
        {
            const std::scoped_lock lock{port_.port_mutex};
            if (port_.generation != generation) {
                return;  // closed meanwhile: dropped with the port's state, no frame prepared
            }
            if (index != -1) {
                const auto& buffer = port_.buffer_slots[index];
                group = port_.groups[buffer.group_index];
                address = buffer.address_left;
            }
        }
        Vulkan::Frame* frame = nullptr;
        if (presenter != nullptr) {
            frame = index == -1 ? presenter->prepare_blank_frame(false)
                                : presenter->prepare_frame(group, address);
        }
        const std::scoped_lock lock{mutex_};
        requests_.push_back({frame, presenter, flip_arg, index, eop, generation, Clock::now()});
    }

    // The oldest request, if it was queued no later than `vblank`.
    std::optional<Request> receive_request(Clock::time_point vblank) {
        const std::scoped_lock lock{mutex_};
        if (requests_.empty() || requests_.front().queued > vblank) {
            return std::nullopt;
        }
        const Request request = requests_.front();
        requests_.pop_front();
        return request;
    }

    void flip(const Request& req) {
        if (req.presenter != nullptr) {
            req.presenter->set_hdr(port_.is_hdr);
            req.presenter->present(req.frame, false, true);
        }

        Port* const port = &port_;
        std::vector<Registration> events;
        std::uint64_t count = 0;
        std::uint64_t tsc = 0;
        int prev_index = -1;
        {
            const std::scoped_lock lock{port->port_mutex};
            if (req.generation != port->generation) {
                return;  // submitted before a close: the reopened port's status stays as it is
            }
            auto& flip_status = port->flip_status;
            count = ++flip_status.count;
            flip_status.process_time = kernel::process_time_usec();
            tsc = flip_status.tsc = kernel::process_counter_nsec();
            flip_status.flip_arg = req.flip_arg;
            flip_status.current_buffer = req.index;
            if (req.eop) {
                --flip_status.gc_queue_num;
            }
            --flip_status.flip_pending_num;
            events = port->flip_events;
            prev_index = std::exchange(port->prev_index, req.index);
        }
        common::log(count <= logged_flips ? common::LogLevel::info : common::LogLevel::debug,
                    channel, "flip {} done: buffer {}, argument {}", count, req.index,
                    req.flip_arg);

        // Trigger the flip events for the port.
        const auto data = static_cast<std::int64_t>(
            (tsc & 0xfff) | (std::uint64_t{1} << 12) |
            (static_cast<std::uint64_t>(req.flip_arg) << 16));
        for (const Registration& r : events) {
            EventQueue::trigger(
                r.equeue, GuestEvent{flip_event_id, video_out_filter, 0, 0, data, r.user_data});
        }

        // Reset the previous flip label.
        if (prev_index != -1) {
            {
                const std::scoped_lock labels{port->vo_mutex};
                port->buffer_labels[prev_index] = 0;
            }
            port->SignalVoLabel();
        }
    }

    // One iteration per vblank, on a fixed grid. A vblank takes a flip only at every
    // (flip_rate + 1)-th count; without one the presenter shows a blank frame while the port is
    // closed, or the last frame again when ImGui has to keep drawing.
    [[noreturn]] void present_thread() {
        common::set_thread_name("video-present");
        Clock::time_point next = Clock::now();
        for (;;) {
            const Clock::time_point vblank = next;
            std::optional<Request> request;
            if (vblank_count_ % (port_.flip_rate + 1) == 0) {
                request = receive_request(vblank);
            }
            if (request) {
                flip(*request);
            } else if (FlipPresenter* const presenter = presenter_) {
                if (!port_.is_open) {
                    presenter->present(presenter->prepare_blank_frame(true), false, false);
                } else if (presenter->must_keep_drawing()) {
                    if (Vulkan::Frame* const frame = presenter->prepare_last_frame()) {
                        presenter->present(frame, true, true);
                    }
                }
            }
            ++vblank_count_;
            next += vblank_period;
            const Clock::time_point now = Clock::now();
            if (now > next + vblank_period) {
                next = now;  // far behind, after a long present: no burst of vblanks
            }
            std::this_thread::sleep_until(next);
        }
    }

    std::atomic<FlipPresenter*> presenter_{nullptr};
    Port port_;
    std::mutex mutex_;
    std::deque<Request> requests_;
    std::uint64_t vblank_count_ = 0;  // the present thread's
    std::atomic<std::uint64_t> submitted_{0};
};

Driver& driver() {
    static Driver& d = *new Driver;  // leaked: the present thread runs until the process ends
    return d;
}

}  // namespace

void start_video_out(FlipPresenter* presenter) {
    driver().set_presenter(presenter);
}

std::int32_t sceVideoOutOpen(std::int32_t user_id, std::int32_t bus_type, std::int32_t index,
                             const void*) {
    if (bus_type != 0 || index != 0) {
        common::log(common::LogLevel::error, channel, "open: bus type {}, index {}", bus_type,
                    index);
        return video_out_invalid_value;
    }
    const std::int32_t handle = driver().open();
    common::log(handle < 0 ? common::LogLevel::error : common::LogLevel::info, channel,
                "open (user {}): {:#x}", user_id, static_cast<std::uint32_t>(handle));
    return handle;
}

std::int32_t sceVideoOutClose(std::int32_t handle) {
    if (driver().get_port(handle) == nullptr) {
        return video_out_invalid_handle;
    }
    driver().close();
    return 0;
}

std::int32_t sceVideoOutSetFlipRate(std::int32_t handle, std::int32_t rate) {
    Port* const port = driver().get_port(handle);
    if (port == nullptr) {
        return video_out_invalid_handle;
    }
    if (rate < 0 || rate > max_flip_rate) {
        return video_out_invalid_value;
    }
    port->flip_rate = rate;
    return 0;
}

std::int32_t sceVideoOutAddFlipEvent(std::uint64_t equeue, std::int32_t handle,
                                     std::uint64_t user_data) {
    common::log(common::LogLevel::info, channel, "add flip event: handle {}", handle);
    if (driver().get_port(handle) == nullptr) {
        return video_out_invalid_handle;
    }
    if (!EventQueue::find(equeue)) {
        return video_out_invalid_event_queue;
    }
    driver().add_flip_event(equeue, user_data);
    return 0;
}

std::int32_t sceVideoOutSetBufferAttribute(void* attr, std::uint32_t pixel_format,
                                           std::uint32_t tiling_mode, std::uint32_t aspect_ratio,
                                           std::uint32_t width, std::uint32_t height,
                                           std::uint32_t pitch_in_pixel) {
    if (attr == nullptr) {
        return video_out_invalid_address;
    }
    common::log(common::LogLevel::info, channel,
                "set buffer attribute: pixelFormat = {:#x}, tilingMode = {}, aspectRatio = {}, "
                "width = {}, height = {}, pitchInPixel = {}",
                pixel_format, tiling_mode, aspect_ratio, width, height, pitch_in_pixel);
    BufferAttribute a{};
    a.pixel_format = static_cast<VO::PixelFormat>(pixel_format);
    a.tiling_mode = static_cast<VO::TilingMode>(tiling_mode);
    a.aspect_ratio = static_cast<s32>(aspect_ratio);
    a.width = width;
    a.height = height;
    a.pitch_in_pixel = pitch_in_pixel;
    a.option = 0;  // SCE_VIDEO_OUT_BUFFER_ATTRIBUTE_OPTION_NONE
    std::memcpy(attr, &a, sizeof a);  // the guest block need not be aligned
    return 0;
}

std::int32_t sceVideoOutRegisterBuffers(std::int32_t handle, std::int32_t start,
                                        const std::uint64_t* addresses, std::int32_t count,
                                        const void* attr) {
    if (addresses == nullptr || attr == nullptr) {
        common::log(common::LogLevel::error, channel, "addresses are null");
        return video_out_invalid_address;
    }
    const Port* const port = driver().get_port(handle);
    if (port == nullptr || !port->is_open) {
        common::log(common::LogLevel::error, channel, "invalid handle {}", handle);
        return video_out_invalid_handle;
    }
    BufferAttribute attribute;
    std::memcpy(&attribute, attr, sizeof attribute);
    return driver().register_buffers(start, addresses, count, attribute);
}

std::int32_t sceVideoOutSubmitFlip(std::int32_t handle, std::int32_t index, std::int32_t mode,
                                   std::int64_t arg) {
    if (driver().get_port(handle) == nullptr) {
        common::log(common::LogLevel::error, channel, "invalid handle {}", handle);
        return video_out_invalid_handle;
    }
    if (index < -1 || index >= static_cast<std::int32_t>(VO::MaxDisplayBuffers)) {
        common::log(common::LogLevel::error, channel, "invalid bufferIndex {}", index);
        return video_out_invalid_index;
    }
    const std::uint64_t number = driver().count_submission();
    common::log(number <= logged_flips ? common::LogLevel::info : common::LogLevel::debug,
                channel, "flip {} submitted: bufferIndex = {}, flipMode = {}, flipArg = {}",
                number, index, mode, arg);
    return driver().submit_flip(index, arg);
}

std::int32_t submit_eop_flip(std::int32_t handle, std::uint32_t index, std::int64_t arg) {
    if (driver().get_port(handle) == nullptr) {
        return video_out_invalid_handle;
    }
    if (index >= static_cast<std::uint32_t>(VO::MaxDisplayBuffers)) {
        return video_out_invalid_index;
    }
    Platform::IrqC::Instance()->RegisterOnce(
        Platform::InterruptId::GfxFlip, [=](Platform::InterruptId irq) {
            ASSERT_MSG(irq == Platform::InterruptId::GfxFlip, "An unexpected IRQ occured");
            Port* const port = driver().get_port(handle);
            ASSERT_MSG(port->buffer_labels[index] == 1, "Out of order flip IRQ");
            const std::int32_t result = driver().submit_flip(static_cast<s32>(index), arg, true);
            ASSERT_MSG(result == 0, "EOP flip submission failed");
        });
    return 0;
}

std::int32_t get_buffer_label_address(std::int32_t handle, std::uintptr_t* address) {
    if (address == nullptr) {
        return video_out_invalid_address;
    }
    Port* const port = driver().get_port(handle);
    if (port == nullptr) {
        return video_out_invalid_handle;
    }
    *address = reinterpret_cast<std::uintptr_t>(port->buffer_labels.data());
    return 16;
}

std::int32_t sceVideoOutGetFlipStatus(std::int32_t handle, void* status) {
    if (status == nullptr) {
        return video_out_invalid_address;
    }
    if (driver().get_port(handle) == nullptr) {
        return video_out_invalid_handle;
    }
    driver().get_flip_status(status);
    return 0;
}

BB_HLE_FUNCTION("libSceVideoOut", "Up36PTk687E", sceVideoOutOpen);
BB_HLE_FUNCTION("libSceVideoOut", "uquVH4-Du78", sceVideoOutClose);
BB_HLE_FUNCTION("libSceVideoOut", "CBiu4mCE1DA", sceVideoOutSetFlipRate);
BB_HLE_FUNCTION("libSceVideoOut", "HXzjK9yI30k", sceVideoOutAddFlipEvent);
BB_HLE_FUNCTION("libSceVideoOut", "i6-sR91Wt-4", sceVideoOutSetBufferAttribute);
BB_HLE_FUNCTION("libSceVideoOut", "w3BY+tAEiQY", sceVideoOutRegisterBuffers);
BB_HLE_FUNCTION("libSceVideoOut", "U46NwOiJpys", sceVideoOutSubmitFlip);
BB_HLE_FUNCTION("libSceVideoOut", "SbU3dwp80lQ", sceVideoOutGetFlipStatus);

}  // namespace bb::hle::video
