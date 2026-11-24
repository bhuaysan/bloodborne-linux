// The frame rate the in-game counter shows (bb-run --show-fps): game frames over a short window,
// with the average and the longest frame time in it. Separate from ImGui so that it is unit-tested
// without a window.
#pragma once

#include <algorithm>
#include <chrono>

namespace bb::common {

class FpsMeter {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::milliseconds window{500};

    struct Reading {
        double fps = 0.0;
        double frame_ms = 0.0;  // average frame time in the window
        double worst_ms = 0.0;  // longest frame time in the window
        bool valid = false;     // false until the first window closes
    };

    // A game frame was presented.
    void frame(Clock::time_point now) {
        if (!started_) {
            started_ = true;
            window_start_ = now;
            last_ = now;
            return;
        }
        const double ms = std::chrono::duration<double, std::milli>(now - last_).count();
        last_ = now;
        ++frames_;
        sum_ms_ += ms;
        worst_ms_ = std::max(worst_ms_, ms);
        update(now);
    }

    // Closes the window once it has run its length, also when no frame came: a game that stops
    // presenting shows 0 FPS instead of the last rate.
    void update(Clock::time_point now) {
        if (!started_ || now - window_start_ < window) {
            return;
        }
        const double elapsed_s = std::chrono::duration<double>(now - window_start_).count();
        reading_.fps = frames_ / elapsed_s;
        reading_.frame_ms = frames_ > 0 ? sum_ms_ / frames_ : 0.0;
        // A frame still in progress counts once it is longer than every finished one.
        const double open_ms = std::chrono::duration<double, std::milli>(now - last_).count();
        reading_.worst_ms = std::max(worst_ms_, frames_ > 0 ? 0.0 : open_ms);
        reading_.valid = true;
        window_start_ = now;
        frames_ = 0;
        sum_ms_ = 0.0;
        worst_ms_ = 0.0;
    }

    Reading reading() const { return reading_; }

private:
    bool started_ = false;
    Clock::time_point window_start_{};
    Clock::time_point last_{};
    unsigned frames_ = 0;
    double sum_ms_ = 0.0;
    double worst_ms_ = 0.0;
    Reading reading_{};
};

}  // namespace bb::common
