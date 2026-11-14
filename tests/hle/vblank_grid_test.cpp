#include "hle/video/vblank_grid.hpp"

#include <chrono>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "hle/video/video_out.hpp"

namespace {

using bb::hle::video::flip_grace;
using bb::hle::video::flip_margin;
using bb::hle::video::vblank_period;
using bb::hle::video::VblankGrid;
using Clock = VblankGrid::Clock;
using namespace std::chrono_literals;

const Clock::time_point t0 = Clock::time_point{} + 1000s;

TEST(VblankGrid, VblanksComeOnePeriodApartAndAreCounted) {
    VblankGrid grid{t0, vblank_period, flip_grace, flip_margin};
    for (std::uint64_t i = 0; i < 4; ++i) {
        const auto vblank = grid.pass(grid.next());
        EXPECT_EQ(vblank.count, i);
        EXPECT_EQ(vblank.time, t0 + static_cast<std::int64_t>(i) * vblank_period);
        EXPECT_EQ(grid.next(), vblank.time + vblank_period);
    }
}

// A present thread that was away for more than a period takes the vblank at `now` and goes on
// from there; one that is a little late keeps the grid.
TEST(VblankGrid, FarBehindStartsAgainWithoutABurst) {
    VblankGrid grid{t0, vblank_period, flip_grace, flip_margin};
    grid.pass(t0);
    const auto late = grid.pass(t0 + vblank_period + 5ms);  // due at t0 + P, 5 ms late
    EXPECT_EQ(late.time, t0 + vblank_period);
    EXPECT_EQ(grid.next(), t0 + 2 * vblank_period);
    const Clock::time_point away = t0 + 10 * vblank_period;
    const auto far = grid.pass(away);
    EXPECT_EQ(far.time, away);
    EXPECT_EQ(far.count, 2u);
    EXPECT_EQ(grid.next(), away + vblank_period);
}

TEST(VblankGrid, FollowsAFlipTakenInTheGraceAndTheMargin) {
    VblankGrid grid{t0, vblank_period, flip_grace, flip_margin};
    const auto vblank = grid.pass(t0);
    EXPECT_EQ(grid.grace_end(vblank), t0 + flip_grace);
    grid.follow(vblank, t0 + 1500us);
    EXPECT_EQ(grid.next(), t0 + 1500us + vblank_period + flip_margin);
    // A thread that woke late takes the flip after the grace: the grid moves by the grace.
    const auto next = grid.pass(grid.next());
    grid.follow(next, next.time + flip_grace + 3ms);
    EXPECT_EQ(grid.next(), next.time + flip_grace + vblank_period + flip_margin);
}

// The flips of a game that paces itself at 30 fps, on the grid of the present thread: what the
// frames of #528 did. `flip_rate` 0, a request queued at each of `arrivals`, one at a time.
struct Flips {
    std::vector<Clock::duration> intervals;  // between flips
    Clock::duration latest{};                // the longest a request waited for its flip
    std::size_t late = 0;                    // intervals over 1.5 frames
    std::size_t off_grid = 0;                // intervals of one frame and over 0.2 ms
};

Flips simulate(const std::vector<Clock::time_point>& arrivals, Clock::time_point grid_start,
               std::chrono::nanoseconds grace, int flip_rate = 0) {
    VblankGrid grid{grid_start, vblank_period, grace, grace == 0ns ? 0ns : flip_margin};
    Flips result;
    std::vector<Clock::time_point> flips;
    std::size_t next = 0;
    while (next < arrivals.size()) {
        const auto vblank = grid.pass(grid.next());
        if (vblank.count % (flip_rate + 1) != 0) {
            continue;
        }
        const auto arrival = arrivals[next];
        if (arrival > grid.grace_end(vblank)) {
            continue;  // not yet
        }
        const Clock::time_point taken = std::max(arrival, vblank.time);
        if (arrival > vblank.time) {
            grid.follow(vblank, taken);
        }
        result.latest = std::max(result.latest, taken - arrival);
        flips.push_back(taken);
        ++next;
    }
    for (std::size_t i = 1; i < flips.size(); ++i) {
        result.intervals.push_back(flips[i] - flips[i - 1]);
        const auto interval = flips[i] - flips[i - 1];
        result.late += interval > 50ms;
        result.off_grid += interval > 2 * vblank_period + 200us && interval < 40ms;
    }
    return result;
}

// Frames every 33.333 ms with a jitter of +-1.5 ms, from a deterministic generator.
std::vector<Clock::time_point> frames_at(Clock::time_point first, int count,
                                         std::chrono::nanoseconds cadence = 2 * vblank_period) {
    std::vector<Clock::time_point> arrivals;
    std::uint32_t state = 12345;
    for (int i = 0; i < count; ++i) {
        state = state * 1664525u + 1013904223u;
        const auto jitter = std::chrono::nanoseconds{
            static_cast<std::int64_t>(state >> 8) % 3'000'000 - 1'500'000};
        arrivals.push_back(first + i * cadence + jitter);
    }
    return arrivals;
}

// The grid sits on the arrivals: a vblank every 33.3 ms falls inside the jitter of a frame.
TEST(VblankGrid, ArrivalsOnTheGridNoLongerMissTheVblank) {
    const auto arrivals = frames_at(t0 + 2s, 450);  // 15 s, the length of a segment
    const Flips fixed = simulate(arrivals, t0 + 2s, 0ns);
    // Without the grace, a third of the flips are 16.7 ms late and the next one is early.
    EXPECT_GT(fixed.late, 450u * 15 / 100);
    EXPECT_GE(fixed.latest, 15ms);

    const Flips followed = simulate(arrivals, t0 + 2s, flip_grace);
    EXPECT_LT(followed.late, 450u * 2 / 100);
    EXPECT_LE(followed.latest, 8ms);  // the grid ends up a few ms after the arrivals
    EXPECT_LT(followed.off_grid, 450u * 2 / 100);  // few frames off the 33.3 ms steps
    for (const auto interval : followed.intervals) {
        // A frame is never shown sooner than the one before it and a period.
        EXPECT_GE(interval, vblank_period);
    }
}

// A game whose frames take a little longer than 33.3 ms pushes its arrivals later and later
// against the grid, which follows them now and then and not at every frame.
TEST(VblankGrid, DriftingArrivalsMoveTheGridRarely) {
    const auto arrivals = frames_at(t0 + 2s, 450, 33'340us);
    const Flips followed = simulate(arrivals, t0 + 2s, flip_grace);
    EXPECT_EQ(followed.late, 0u);
    EXPECT_LT(followed.off_grid, 450u * 2 / 100);
    const Flips fixed = simulate(arrivals, t0 + 2s, 0ns);
    EXPECT_GT(fixed.late, 450u * 5 / 100);
}

// A grid half a period away from the arrivals stays where it is: the flips wait as before.
TEST(VblankGrid, ArrivalsAwayFromTheGridKeepTheirPhase) {
    const auto arrivals = frames_at(t0 + 2s, 450);
    const Flips fixed = simulate(arrivals, t0 + 2s - 8ms, 0ns);
    const Flips followed = simulate(arrivals, t0 + 2s - 8ms, flip_grace);
    EXPECT_EQ(followed.late, 0u);
    EXPECT_EQ(fixed.late, 0u);
    EXPECT_EQ(followed.intervals, fixed.intervals);
    EXPECT_EQ(followed.latest, fixed.latest);
}

// With flip rate 1 a flip is taken every second vblank, 33.3 ms apart at least.
TEST(VblankGrid, FlipRateStillSpacesTheFlips) {
    // Frames every 20 ms, faster than a flip at rate 1 takes them.
    std::vector<Clock::time_point> arrivals;
    for (int i = 0; i < 60; ++i) {
        arrivals.push_back(t0 + 1s + i * 20ms);
    }
    const Flips flips = simulate(arrivals, t0 + 1s, flip_grace, 1);
    ASSERT_FALSE(flips.intervals.empty());
    for (const auto interval : flips.intervals) {
        EXPECT_GE(interval, 2 * vblank_period);
    }
}

}  // namespace
