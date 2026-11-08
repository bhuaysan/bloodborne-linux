#include <chrono>
#include <cstdint>

#include <gtest/gtest.h>

#include "hle/net/net.hpp"
#include "hle/system/system.hpp"

namespace {

using Clock = std::chrono::steady_clock;

TEST(HleBlocking, AudioOutOutputBlocksForOneBuffer) {
    const auto start = Clock::now();
    EXPECT_EQ(bb::hle::system::sceAudioOutOutput(1, nullptr), 0);
    const auto took = Clock::now() - start;
    // 256 samples at 48 kHz are 5.33 ms.
    EXPECT_GE(took, std::chrono::microseconds(5333));
    EXPECT_LT(took, std::chrono::milliseconds(500));
}

TEST(HleBlocking, EpollWaitBlocksUntilItsTimeout) {
    const auto start = Clock::now();
    EXPECT_EQ(bb::hle::net::sceNetEpollWait(1, nullptr, 8, 20'000), 0);
    const auto took = Clock::now() - start;
    EXPECT_GE(took, std::chrono::milliseconds(20));
    EXPECT_LT(took, std::chrono::milliseconds(1000));
}

TEST(HleBlocking, EpollWaitWithZeroTimeoutReturnsAtOnce) {
    const auto start = Clock::now();
    EXPECT_EQ(bb::hle::net::sceNetEpollWait(1, nullptr, 8, 0), 0);
    EXPECT_LT(Clock::now() - start, std::chrono::milliseconds(100));
}

}  // namespace
