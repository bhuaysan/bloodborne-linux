// The vblank grid of video out's present thread (#528).
//
// Vblanks are virtual here, a 60 Hz grid that only the flips and the idle presents follow. A
// flip queued just after a vblank used to wait a whole period for the next one. The game paces
// itself by its own clock (the frame pacer at 0x2034770 waits for 1/30 s), so its flips keep
// the same phase against the grid for seconds. When that phase sits within the jitter of a
// frame (a millisecond or two) of a vblank, a third of the flips missed it and showed 16.7 ms
// late. The grid now gives a flip that comes a little after a vblank (up to `grace`) that
// vblank and follows it: the next vblank is one period and a margin after the flip, so that
// the next frames, with their jitter, come before it. A grid that sits within the jitter of the
// arrivals moves later until the arrivals come before the vblanks, and stays there. It never
// moves earlier and a flip is never shown later than the next vblank of the grid it found, so
// it adds no latency to the flip; the later grid can show a later frame a little later than the
// old one would have (up to the grace and the margin, once per move). A flip is still taken
// only at every (flip rate + 1)-th vblank and, unless the present thread woke more than the
// grace late, a period or more after the last. When a vblank finds several flips queued it shows
// the newest, and the grid follows that one (video_out.cpp, #608).
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace bb::hle::video {

class VblankGrid {
public:
    using Clock = std::chrono::steady_clock;

    struct Vblank {
        Clock::time_point time;   // when the vblank was due
        std::uint64_t count = 0;  // its number, from 0
    };

    VblankGrid(Clock::time_point first, std::chrono::nanoseconds period,
               std::chrono::nanoseconds grace, std::chrono::nanoseconds margin)
        : next_(first), period_(period), grace_(grace), margin_(margin) {}

    // When the next vblank is due.
    Clock::time_point next() const { return next_; }

    // The vblank due at next() has come, `now` (now >= next()). Moves on to the next one; a
    // present thread that fell more than a period behind starts again at `now`, with no burst of
    // vblanks to catch up.
    Vblank pass(Clock::time_point now) {
        if (now > next_ + period_) {
            next_ = now;
        }
        const Vblank vblank{next_, count_++};
        next_ += period_;
        return vblank;
    }

    // Until when a flip queued after `vblank` still takes it.
    Clock::time_point grace_end(const Vblank& vblank) const { return vblank.time + grace_; }

    // A flip that was queued after `vblank` was taken at `taken`, within the grace: the grid
    // follows it and the margin. The shift is the lateness, at most the grace, and the margin.
    void follow(const Vblank& vblank, Clock::time_point taken) {
        next_ = std::min(taken, grace_end(vblank)) + period_ + margin_;
    }

private:
    Clock::time_point next_;
    std::chrono::nanoseconds period_;
    std::chrono::nanoseconds grace_;
    std::chrono::nanoseconds margin_;
    std::uint64_t count_ = 0;
};

}  // namespace bb::hle::video
