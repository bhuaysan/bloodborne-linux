// The frame pacer (docs/design/frame-rate.md, "The wait").
#include <immintrin.h>

#include <atomic>
#include <cstdint>

#include "game/fd4.hpp"
#include "game/frame/frame.hpp"
#include "game/image.hpp"
#include "hle/hle.hpp"
#include "hle/kernel/time.hpp"
#include "hook/hook.hpp"

namespace bb::game {
namespace {

std::atomic<FrameObserver> frame_observer{nullptr};

// The foreground wait sleeps until this long before its end and spins only the rest (#656), so
// the end of the wait keeps the precision of the spin: the sleep's wake-up latency (the timer
// slack of 50 us and the scheduler) stays inside the margin. Measured in #656.
constexpr float spin_margin_s = 0.002f;

struct Timeval {
    std::int64_t sec;
    std::int64_t usec;
};

// libScePosix gettimeofday in microseconds, wrapping as the game's 64-bit arithmetic does.
std::uint64_t now_us() {
    using Gettimeofday = int(Timeval*, void*);
    static const auto gettimeofday =
        reinterpret_cast<Gettimeofday*>(hle::import_address("libScePosix", "n88vx3C5nW8"));
    Timeval tv{};
    gettimeofday(&tv, nullptr);
    return static_cast<std::uint64_t>(tv.sec) * 1000000 + static_cast<std::uint64_t>(tv.usec);
}

void usleep_us(std::int32_t us) {
    using Usleep = int(std::uint32_t);
    static const auto usleep =
        reinterpret_cast<Usleep*>(hle::import_address("libkernel", "1jfXLRVzisc"));
    usleep(static_cast<std::uint32_t>(us));
}

// float seconds * 1e6 in double, truncated to unsigned as the game's compiler does: cvttsd2si,
// on x - 2^63 with the top bit flipped for x >= 2^63 (NaN and the out-of-range give the
// "indefinite" 2^63).
std::uint64_t to_us(float seconds) {
    const double us = static_cast<double>(seconds) * 1000000.0;
    constexpr double two63 = 9223372036854775808.0;
    const auto truncate = [](double v) {
        return static_cast<std::uint64_t>(_mm_cvttsd_si64(_mm_set_sd(v)));
    };
    return us >= two63 ? truncate(us - two63) ^ 0x8000000000000000ULL : truncate(us);
}

// minss a, b: a when a < b, else b (also when either is NaN).
float min_ss(float a, float b) {
    return a < b ? a : b;
}

}  // namespace

FrameObserver set_frame_observer(FrameObserver observer) {
    return frame_observer.exchange(observer, std::memory_order_acq_rel);
}

// 0x02034770
void flipper_wait_frame(SprjFlipper* self) {
    if (const FrameObserver observer = frame_observer.load(std::memory_order_acquire)) {
        observer();
    }
    void*& window = global<void*>(0x05540500);
    if (window == nullptr) {
        singleton_missing(0x0493b00c);  // "SprjWindow"
    }
    const bool foreground = fn<bool(void*)>(0x00fe3980)(window);  // always true

    const std::uint8_t heavy = self->heavy;
    self->heavy_last = heavy;
    self->heavy = 0;
    const std::uint32_t mode = heavy != 0 ? self->flip_mode_heavy : self->flip_mode;
    // Mode: present interval, tearing, frame time and foreground skip maximum; the background
    // skip maximum is 30 in every mode. Modes 3 and 4 take frame_step(), which is the game's 1/30
    // at 30 FPS.
    const auto set_mode = [self](std::int32_t interval, std::uint8_t tearing, float frame_time,
                                 std::uint32_t foreground_skip) {
        self->present_interval = interval;
        self->tearing = tearing;
        self->target_frame_time = frame_time;
        self->foreground_skip_max = foreground_skip;
        self->background_skip_max = 30;
    };
    constexpr float thirtieth = 1.0f / 30.0f;
    switch (mode) {
    case 0: set_mode(2, 1, thirtieth, 1); break;            // 30FPS
    case 1: set_mode(2, 1, thirtieth, 0); break;            // 30FPS_withoutSkip
    case 2: set_mode(1, 1, 1.0f / 60.0f, 0); break;         // 60FPS_withoutSkip
    case 3: set_mode(1, 0, frame_step(), 1); break;         // 30FPS_withoutTearing
    case 4: set_mode(1, 0, frame_step(), 0); break;         // 30FPS_withoutSkipTearing
    default: break;
    }
    const bool forced = self->force_default_mode != 0;
    if (forced) {
        set_mode(1, 0, frame_step(), 0);
        self->background_skip_max = 0;
        self->force_default_mode = 0;
    }
    if (self->foreground_skip_max_debug >= 0) {
        self->foreground_skip_max = static_cast<std::uint32_t>(self->foreground_skip_max_debug);
    }
    if (self->background_skip_max_debug >= 0) {
        self->background_skip_max = static_cast<std::uint32_t>(self->background_skip_max_debug);
    }
    if (self->clear_skip_once != 0) {
        self->foreground_skip_max = 0;
        self->background_skip_max = 0;
        self->clear_skip_once = 0;
    }
    if (self->no_skip != 0) {
        self->foreground_skip_max = 0;
        self->background_skip_max = 0;
    }

    self->prev_time_us = self->now_time_us;
    std::uint64_t now = now_us();
    self->now_time_us = now;
    const std::uint64_t frame_us = to_us(self->target_frame_time);

    // Frame skip: the durations of the last frames, as long as they were late, at most
    // skip_max of them. After a late frame the wait makes up for them, but waits at least a
    // third of a frame.
    const std::uint32_t skip_max =
        foreground ? self->foreground_skip_max : self->background_skip_max;
    std::uint32_t counted = 0;
    std::uint64_t late_sum = 0;
    for (std::uint32_t back = 0; counted < skip_max; --back) {
        const FlipperFrameRecord& record = self->records[(self->record_index + back) & 31];
        late_sum += record.duration_us;
        ++counted;
        if (record.late == 0) {
            break;
        }
    }
    std::uint64_t wait_us = frame_us;
    const std::uint64_t budget = std::uint64_t{counted + 1} * frame_us;
    if (self->late != 0 && late_sum + frame_us > budget) {
        const std::uint64_t third = frame_us / 3;
        wait_us = third;
        if (budget > late_sum) {
            std::uint64_t left = budget - late_sum;
            if (frame_us < left) {
                left = frame_us;
            }
            wait_us = third > left ? third : left;
        }
    }

    if (!forced && self->skip_wait == 0) {
        const float wait = static_cast<float>(wait_us) / 1000000.0f;
        float remaining = wait - static_cast<float>(now - self->prev_time_us) / 1000000.0f;
        // Busy wait in the foreground; in the background sleep while more than 5 ms remain.
        // Unlike the game, the foreground sleeps first, until spin_margin_s before the end the
        // game computes, and spins from there on.
        if (!(0.0f > remaining)) {
            for (;;) {
                const float left = min_ss(wait, remaining);
                if (!(left > 0.0f)) {
                    break;
                }
                if (const float sleep_s = left - spin_margin_s; foreground && sleep_s > 0.0f) {
                    hle::kernel::sleep_until_wall_clock(now + to_us(sleep_s));
                }
                if (!foreground && left > 0.001f && left > 0.005f) {
                    const std::int32_t us =
                        _mm_cvttss_si32(_mm_set_ss((left + -0.005f) * 1000.0f * 1000.0f));
                    if (us > 0) {
                        usleep_us(us);
                    }
                }
                now = now_us();
                self->now_time_us = now;
                remaining = wait + static_cast<float>(now - self->prev_time_us) / -1000000.0f;
                if (0.0f > remaining) {
                    break;
                }
            }
        }
    }
    if (self->skip_wait != 0) {
        self->late = 1;
    }

    // Record the frame. It is late when it took longer than the frame time and 1 %, or, while
    // making up, than the frames it makes up for, unless the skips are used up (all skip_max
    // counted while late, else skip_max 0), which counts as on time. With skip_wait it is always
    // late.
    const std::uint32_t index = (self->record_index + 1) & 31;
    self->record_index = index;
    self->records[index].duration_us = now - self->prev_time_us;
    const std::uint8_t was_late = self->late;
    self->records[index].late = was_late;
    const std::uint64_t took = self->now_time_us - self->prev_time_us;
    const float took_s = static_cast<float>(took) / 1000000.0f;
    self->delta_time = took_s;
    const bool skips_used_up = was_late != 0 ? counted >= skip_max : skip_max == 0;
    const std::uint64_t tolerance_us = to_us(self->target_frame_time * 0.01f);
    const std::uint64_t allowed = std::uint64_t{counted + 1} * to_us(self->target_frame_time) +
                                  tolerance_us;
    bool on_time = took + late_sum < allowed || skips_used_up;
    if (self->skip_wait != 0) {
        on_time = false;
    }
    self->late = on_time ? 0 : 1;
    self->present_interval = on_time ? self->present_interval : 0;

    // The last 16 frame times and the frame rate over them, for the debug menu.
    float sum = 0.0f;
    for (int i = 0; i < 15; ++i) {
        self->recent_delta[i] = self->recent_delta[i + 1];
        sum += self->recent_delta[i];
    }
    self->recent_delta[15] = took_s;
    sum += took_s;
    self->fps = sum > 0.001f ? 16.0f / sum : 0.0f;

    self->skip_wait = 0;
    if (auto* const target = global<std::uint8_t*>(0x05540e00);
        target != nullptr && self->request_0x274 != 0) {
        target[0x4c] = 1;
        target[0x4d] = 1;
        *reinterpret_cast<std::int32_t*>(target + 0x50) = 60;
    }
    self->request_0x274 = 0;
}

}  // namespace bb::game

BB_REPLACE(0x02034770, bb::game::flipper_wait_frame);
