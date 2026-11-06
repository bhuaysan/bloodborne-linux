// The main-loop probe of bb-run (docs/design/m3-core-os.md, "Goal: main loop reached"). The game
// has reached its main loop once the frame pacer has read the wall clock 60 times while main has
// not returned. libScePosix gettimeofday reports the return address of every call
// (bb::hle::kernel::set_gettimeofday_observer); the probe counts those inside the frame pacer.
#pragma once

#include <atomic>
#include <cstdint>

namespace bb::tools {

// The frame pacer: its link address and its length, from its FDE. It reads the wall clock through
// libScePosix gettimeofday once or more per tick, and the tick runs only in main's loop.
inline constexpr std::uint64_t frame_pacer = 0x2034770;
inline constexpr std::uint64_t frame_pacer_size = 0x820;

// The number of frame pacer calls that means the main loop is reached.
inline constexpr unsigned main_loop_calls = 60;

// Counts gettimeofday calls whose return address lies in [begin, end), until main returns. Any
// thread may call it.
class MainLoopProbe {
public:
    MainLoopProbe(std::uintptr_t begin, std::uintptr_t end, unsigned threshold = main_loop_calls)
        : begin_(begin), end_(end), threshold_(threshold) {}

    // One gettimeofday call. True for exactly one call over the probe's life: the one that is
    // the threshold-th to count. Calls from outside the range and after main_returned don't count.
    bool count(std::uintptr_t return_address);

    // main has returned (its exit handler runs): no later call counts.
    void main_returned() { main_returned_.store(true, std::memory_order_release); }

    // The calls that counted so far (at most a few above the threshold).
    unsigned calls() const { return calls_.load(std::memory_order_acquire); }

private:
    std::uintptr_t begin_;
    std::uintptr_t end_;
    unsigned threshold_;
    std::atomic<unsigned> calls_{0};
    std::atomic<bool> main_returned_{false};
};

}  // namespace bb::tools
