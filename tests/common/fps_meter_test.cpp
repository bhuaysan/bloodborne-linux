// The in-game frame rate counter's arithmetic (src/common/fps_meter.hpp).

#include <chrono>

#include <gtest/gtest.h>

#include "common/fps_meter.hpp"

namespace {

using bb::common::FpsMeter;
using namespace std::chrono_literals;

const FpsMeter::Clock::time_point t0{};

// Presents `count` frames `step` apart after `start`, returns the last one's time.
FpsMeter::Clock::time_point run(FpsMeter& meter, FpsMeter::Clock::time_point start,
                                FpsMeter::Clock::duration step, int count) {
    auto t = start;
    for (int i = 0; i < count; ++i) {
        t += step;
        meter.frame(t);
    }
    return t;
}

TEST(FpsMeter, NoReadingBeforeTheFirstWindow) {
    FpsMeter meter;
    EXPECT_FALSE(meter.reading().valid);
    meter.frame(t0);
    run(meter, t0, 16ms, 10);
    EXPECT_FALSE(meter.reading().valid);
}

TEST(FpsMeter, SteadyRates) {
    for (const int hz : {30, 60, 120, 144}) {
        FpsMeter meter;
        meter.frame(t0);
        const auto step = std::chrono::duration_cast<FpsMeter::Clock::duration>(
            std::chrono::duration<double>(1.0 / hz));
        run(meter, t0, step, hz);  // one second: two windows
        const auto r = meter.reading();
        ASSERT_TRUE(r.valid) << hz;
        EXPECT_NEAR(r.fps, hz, 0.5) << hz;
        EXPECT_NEAR(r.frame_ms, 1000.0 / hz, 0.01) << hz;
        EXPECT_NEAR(r.worst_ms, 1000.0 / hz, 0.01) << hz;
    }
}

TEST(FpsMeter, AHitchShowsAsWorstFrame) {
    FpsMeter meter;
    meter.frame(t0);
    auto t = run(meter, t0, 10ms, 20);
    t += 100ms;  // one long frame
    meter.frame(t);
    run(meter, t, 10ms, 20);  // window closes at 500 ms
    const auto r = meter.reading();
    ASSERT_TRUE(r.valid);
    EXPECT_NEAR(r.worst_ms, 100.0, 0.01);
    EXPECT_NEAR(r.fps, 41 / 0.5, 0.01);
    EXPECT_NEAR(r.frame_ms, 500.0 / 41, 0.01);
}

TEST(FpsMeter, NoFramesReadsZero) {
    FpsMeter meter;
    meter.frame(t0);
    meter.update(t0 + 400ms);
    EXPECT_FALSE(meter.reading().valid);
    meter.update(t0 + 2s);
    const auto r = meter.reading();
    ASSERT_TRUE(r.valid);
    EXPECT_EQ(r.fps, 0.0);
    EXPECT_NEAR(r.worst_ms, 2000.0, 0.01);
}

TEST(FpsMeter, UpdateBeforeAnyFrameDoesNothing) {
    FpsMeter meter;
    meter.update(t0 + 10s);
    EXPECT_FALSE(meter.reading().valid);
}

TEST(FpsMeter, RecoversAfterAPause) {
    FpsMeter meter;
    meter.frame(t0);
    auto t = t0 + 5s;  // the window was hidden
    meter.frame(t);
    EXPECT_NEAR(meter.reading().fps, 1 / 5.0, 1e-9);
    run(meter, t, 16667us, 31);
    EXPECT_NEAR(meter.reading().fps, 60.0, 1.0);
}

}  // namespace
