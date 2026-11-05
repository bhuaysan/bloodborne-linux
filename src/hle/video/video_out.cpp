#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common/log.hpp"
#include "hle/hle.hpp"
#include "hle/kernel/equeue.hpp"
#include "hle/video/video.hpp"

namespace bb::hle::video {
namespace {

constexpr std::string_view channel = "hle.libSceVideoOut";

// The first flips are logged at info, the rest at debug.
constexpr std::uint64_t logged_flips = 3;

using Clock = std::chrono::steady_clock;

struct Flip {
    std::int32_t index = 0;
    std::int64_t arg = 0;
    Clock::time_point due;
};

struct Registration {
    std::uint64_t equeue = 0;
    std::uint64_t user_data = 0;
};

struct Port {
    std::deque<Flip> pending;
    std::uint64_t completed = 0;
    std::uint64_t submitted = 0;
    Clock::time_point last_due;  // of the newest flip, pending or not
    std::vector<Registration> registrations;
};

// The first tick strictly after `t`. A tick is no whole number of clock units, so it is rounded
// up: rounding down would put it before the tick, and the next call would return it again.
Clock::time_point next_tick(Clock::time_point t) {
    const auto ticks = std::chrono::floor<Tick>(t.time_since_epoch()) + Tick(1);
    return Clock::time_point(std::chrono::ceil<Clock::duration>(ticks));
}

class VideoOut {
public:
    std::int32_t open() {
        const std::lock_guard lock(mutex_);
        const std::int32_t handle = next_handle_++;
        ports_.try_emplace(handle);
        return handle;
    }

    bool close(std::int32_t handle) {
        const std::lock_guard lock(mutex_);
        return ports_.erase(handle) != 0;
    }

    bool add_flip_event(std::int32_t handle, std::uint64_t equeue, std::uint64_t user_data) {
        const std::lock_guard lock(mutex_);
        Port* port = find(handle);
        if (port == nullptr) {
            return false;
        }
        const auto same = std::ranges::find(port->registrations, equeue, &Registration::equeue);
        if (same != port->registrations.end()) {
            same->user_data = user_data;
        } else {
            port->registrations.push_back({equeue, user_data});
        }
        return true;
    }

    // The flip's number on its port, from 1, or 0 if the port is not open.
    std::uint64_t submit(std::int32_t handle, std::int32_t index, std::int64_t arg) {
        std::uint64_t number = 0;
        {
            const std::lock_guard lock(mutex_);
            Port* port = find(handle);
            if (port == nullptr) {
                return 0;
            }
            const Clock::time_point due = next_tick(std::max(Clock::now(), port->last_due));
            port->last_due = due;
            port->pending.push_back({index, arg, due});
            number = ++port->submitted;
            if (!thread_started_) {
                thread_started_ = true;
                std::thread([this] { run(); }).detach();
            }
        }
        changed_.notify_one();
        return number;
    }

    // The number of completed flips, or nullopt if the port is not open.
    std::optional<std::uint64_t> completed(std::int32_t handle) {
        const std::lock_guard lock(mutex_);
        Port* port = find(handle);
        return port == nullptr ? std::nullopt : std::optional(port->completed);
    }

private:
    Port* find(std::int32_t handle) {
        const auto it = ports_.find(handle);
        return it == ports_.end() ? nullptr : &it->second;
    }

    // The flip thread: completes each port's oldest flip once its tick has come and triggers
    // the flip events outside the lock.
    [[noreturn]] void run() {
        common::set_thread_name("video-flip");
        std::vector<std::pair<std::uint64_t, GuestEvent>> events;
        std::unique_lock lock(mutex_);
        for (;;) {
            std::optional<Clock::time_point> wake;
            const Clock::time_point now = Clock::now();
            for (auto& [handle, port] : ports_) {
                if (port.pending.empty()) {
                    continue;
                }
                const Flip& flip = port.pending.front();
                if (flip.due <= now) {
                    ++port.completed;
                    for (const Registration& r : port.registrations) {
                        events.emplace_back(r.equeue,
                                            GuestEvent{static_cast<std::uint64_t>(handle),
                                                       flip_event_kind, 0,
                                                       static_cast<std::uint32_t>(flip.index),
                                                       flip.arg, r.user_data});
                    }
                    port.pending.pop_front();
                }
                if (!port.pending.empty()) {
                    const Clock::time_point due = port.pending.front().due;
                    wake = wake ? std::min(*wake, due) : due;
                }
            }
            if (!events.empty()) {
                lock.unlock();
                for (const auto& [equeue, event] : events) {
                    EventQueue::trigger(equeue, event);
                }
                events.clear();
                lock.lock();
                continue;
            }
            if (wake) {
                changed_.wait_until(lock, *wake);
            } else {
                changed_.wait(lock);
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable changed_;
    std::unordered_map<std::int32_t, Port> ports_;
    std::int32_t next_handle_ = 1;
    bool thread_started_ = false;
};

VideoOut& video_out() {
    static VideoOut& v = *new VideoOut;  // leaked: the flip thread runs until the process ends
    return v;
}

}  // namespace

std::int32_t sceVideoOutOpen(std::int32_t user_id, std::int32_t type, std::int32_t index,
                             const void* param) {
    const std::int32_t handle = video_out().open();
    common::log(common::LogLevel::info, channel, "open ({}, {}, {}, {}): port {}", user_id, type,
                index, param, handle);
    return handle;
}

std::int32_t sceVideoOutClose(std::int32_t handle) {
    return video_out().close(handle) ? 0 : video_out_error;
}

std::int32_t sceVideoOutAddFlipEvent(std::uint64_t equeue, std::int32_t handle,
                                     std::uint64_t user_data) {
    if (!EventQueue::find(equeue)) {
        return video_out_error;
    }
    return video_out().add_flip_event(handle, equeue, user_data) ? 0 : video_out_error;
}

std::int32_t sceVideoOutSubmitFlip(std::int32_t handle, std::int32_t index, std::int32_t mode,
                                   std::int64_t arg) {
    const std::uint64_t number = video_out().submit(handle, index, arg);
    if (number == 0) {
        return video_out_error;
    }
    common::log(number <= logged_flips ? common::LogLevel::info : common::LogLevel::debug,
                channel, "port {}: flip {} to buffer {}, mode {}, argument {}", handle, number,
                index, mode, arg);
    return 0;
}

std::int32_t sceVideoOutGetFlipStatus(std::int32_t handle, void* status) {
    const auto completed = video_out().completed(handle);
    if (!completed || status == nullptr) {
        return video_out_error;
    }
    std::memcpy(static_cast<char*>(status) + flip_count_offset, &*completed, sizeof *completed);
    return 0;
}

BB_HLE_FUNCTION("libSceVideoOut", "Up36PTk687E", sceVideoOutOpen);
BB_HLE_FUNCTION("libSceVideoOut", "uquVH4-Du78", sceVideoOutClose);
BB_HLE_FUNCTION("libSceVideoOut", "HXzjK9yI30k", sceVideoOutAddFlipEvent);
BB_HLE_FUNCTION("libSceVideoOut", "U46NwOiJpys", sceVideoOutSubmitFlip);
BB_HLE_FUNCTION("libSceVideoOut", "SbU3dwp80lQ", sceVideoOutGetFlipStatus);

}  // namespace bb::hle::video
