// The controller's quit chord of bb-run (src/common/quit_chord.hpp): Back and Start held together.

#include <chrono>

#include <gtest/gtest.h>

#include "common/quit_chord.hpp"

namespace {

using bb::common::QuitChord;
using namespace std::chrono_literals;

const QuitChord::Clock::time_point t0{};

TEST(QuitChord, BothHeldForTwoSecondsEndsTheRun) {
    QuitChord chord;
    chord.back(true, t0);
    chord.start(true, t0 + 300ms);  // the hold counts from here
    EXPECT_FALSE(chord.held(t0 + 2200ms));
    EXPECT_TRUE(chord.held(t0 + 2300ms));
}

TEST(QuitChord, OneButtonAloneNeverEndsIt) {
    QuitChord chord;
    chord.back(true, t0);
    EXPECT_FALSE(chord.held(t0 + 60s));
    QuitChord other;
    other.start(true, t0);
    EXPECT_FALSE(other.held(t0 + 60s));
}

TEST(QuitChord, ReleasingEitherButtonStartsTheHoldAgain) {
    QuitChord chord;
    chord.back(true, t0);
    chord.start(true, t0);
    chord.start(false, t0 + 1500ms);
    EXPECT_FALSE(chord.held(t0 + 2500ms));
    chord.start(true, t0 + 3s);
    EXPECT_FALSE(chord.held(t0 + 4500ms));
    EXPECT_TRUE(chord.held(t0 + 5s));
    chord.back(false, t0 + 5s);
    EXPECT_FALSE(chord.held(t0 + 10s));
}

TEST(QuitChord, ARepeatedPressDoesNotRestartTheHold) {
    QuitChord chord;
    chord.back(true, t0);
    chord.start(true, t0);
    chord.back(true, t0 + 1500ms);  // a second controller's Back, say
    EXPECT_TRUE(chord.held(t0 + 2s));
}

TEST(QuitChord, ARemovedControllerDropsWhatItHeld) {
    QuitChord chord;
    chord.back(true, t0);  // its release never comes
    chord.reset();
    chord.start(true, t0 + 1s);
    EXPECT_FALSE(chord.held(t0 + 10s));
}

}  // namespace
