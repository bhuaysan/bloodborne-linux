// The main-loop probe of bb-run (docs/design/m3-core-os.md, "Goal: main loop reached"). The game
// has reached its main loop once the frame pacer has run 60 times while main has not returned.
// The pacer is replaced (src/game/frame/flipper.cpp) and reports every call
// (bb::game::set_frame_observer); it runs only in main's loop.
#pragma once

#include <atomic>

namespace bb::tools {

// The number of frame pacer calls that means the main loop is reached.
inline constexpr unsigned main_loop_calls = 60;

// Counts frame pacer calls until main returns. Any thread may call it.
class MainLoopProbe {
public:
    explicit MainLoopProbe(unsigned threshold = main_loop_calls) : threshold_(threshold) {}

    // One frame pacer call. True for exactly one call over the probe's life: the one that is the
    // threshold-th to count. Calls after main_returned don't count.
    bool count();

    // main has returned (its exit handler runs): no later call counts.
    void main_returned() { main_returned_.store(true, std::memory_order_release); }

    // The calls that counted so far (at most a few above the threshold).
    unsigned calls() const { return calls_.load(std::memory_order_acquire); }

private:
    unsigned threshold_;
    std::atomic<unsigned> calls_{0};
    std::atomic<bool> main_returned_{false};
};

}  // namespace bb::tools
