// The controller's way out of bb-run (#757): Back and Start held together for two seconds end the
// run as closing its window does. In Steam's Game Mode (gamescope) the window has no close button
// and the Steam menu's "Exit game" ends the launcher too, so without this a player with only a
// controller can't get back to the launcher. The PS4 keeps Share for the system, so the game sees
// Back only as a touch-pad press and Start as Options: holding both opens a menu for a moment.
// Separate from SDL so that it is unit-tested without a window.
#pragma once

#include <chrono>

namespace bb::common {

class QuitChord {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::milliseconds hold{2000};

    // A press or release of Back or Start, on any controller.
    void back(bool down, Clock::time_point now) { set(back_, down, now); }
    void start(bool down, Clock::time_point now) { set(start_, down, now); }

    // Both buttons have been down together for `hold`.
    bool held(Clock::time_point now) const {
        return back_ && start_ && now - since_ >= hold;
    }

private:
    void set(bool& button, bool down, Clock::time_point now) {
        const bool was = back_ && start_;
        button = down;
        if (!was && back_ && start_) {
            since_ = now;  // the hold starts when the second button goes down
        }
    }

    bool back_ = false;
    bool start_ = false;
    Clock::time_point since_{};
};

}  // namespace bb::common
