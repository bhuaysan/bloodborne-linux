#include "common/frame_profile.hpp"

#include <set>
#include <string_view>
#include <thread>

#include <gtest/gtest.h>

#include "common/log.hpp"

namespace {

namespace fp = bb::common::frame_profile;
using bb::common::LogFilter;

class FrameProfile : public ::testing::Test {
protected:
    void SetUp() override { bb::common::set_log_filter(LogFilter{}); (void)fp::take(); }
    void TearDown() override { bb::common::set_log_filter(LogFilter{}); }
};

TEST_F(FrameProfile, OffByDefaultAndRecordsNothing) {
    EXPECT_FALSE(fp::enabled());
    {
        fp::Scope scope(fp::Part::draw);
    }
    fp::add(fp::Part::draw, 5);  // add() itself always counts, Scope is what checks the switch
    const fp::Totals totals = fp::take();
    EXPECT_EQ(totals.calls[static_cast<std::size_t>(fp::Part::draw)], 1u);
}

TEST_F(FrameProfile, ProfileChannelAtDebugTurnsItOn) {
    bb::common::set_log_filter(LogFilter::parse("profile=debug"));
    EXPECT_TRUE(fp::enabled());
    bb::common::set_log_filter(LogFilter::parse("debug"));
    EXPECT_TRUE(fp::enabled());
    bb::common::set_log_filter(LogFilter::parse("profile=info"));
    EXPECT_FALSE(fp::enabled());
    bb::common::set_log_filter(LogFilter::parse("hle=trace"));
    EXPECT_FALSE(fp::enabled());
}

TEST_F(FrameProfile, ScopeAddsTimeAndACallWhenOn) {
    bb::common::set_log_filter(LogFilter::parse("profile=debug"));
    {
        fp::Scope scope(fp::Part::pipeline);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const fp::Totals totals = fp::take();
    const auto i = static_cast<std::size_t>(fp::Part::pipeline);
    EXPECT_EQ(totals.calls[i], 1u);
    EXPECT_GE(totals.ns[i], 5'000'000u);
    EXPECT_EQ(fp::take().calls[i], 0u);  // take() cleared it
}

TEST_F(FrameProfile, ThreadsAddUp) {
    std::thread a([] { for (int i = 0; i < 1000; ++i) fp::add(fp::Part::fault, 10); });
    std::thread b([] { for (int i = 0; i < 1000; ++i) fp::add(fp::Part::fault, 10); });
    a.join();
    b.join();
    const fp::Totals totals = fp::take();
    const auto i = static_cast<std::size_t>(fp::Part::fault);
    EXPECT_EQ(totals.calls[i], 2000u);
    EXPECT_EQ(totals.ns[i], 20000u);
}

TEST_F(FrameProfile, LineNamesOnlyPartsWithCalls) {
    fp::Totals totals;
    totals.ns[static_cast<std::size_t>(fp::Part::upload)] = 2'500'000;
    totals.calls[static_cast<std::size_t>(fp::Part::upload)] = 3;
    totals.ns[static_cast<std::size_t>(fp::Part::fault)] = 1'999;
    totals.calls[static_cast<std::size_t>(fp::Part::fault)] = 7;
    EXPECT_EQ(fp::format_frame(totals, 123, 33'333'000),
              "Frame profile: at 123 ns, 33333 us since the last, upload=2500 us/3, "
              "fault=1 us/7");
}

TEST_F(FrameProfile, EveryPartHasOneName) {
    std::set<std::string_view> seen;
    for (unsigned i = 0; i < static_cast<unsigned>(fp::Part::count); ++i) {
        const auto name = fp::part_name(static_cast<fp::Part>(i));
        EXPECT_FALSE(name.empty()) << i;
        EXPECT_TRUE(seen.insert(name).second) << name;
    }
}

TEST_F(FrameProfile, NamesFollowTheEnumerators) {
    EXPECT_EQ(fp::part_name(fp::Part::commands), "commands");
    EXPECT_EQ(fp::part_name(fp::Part::sync), "sync");
    EXPECT_EQ(fp::part_name(fp::Part::inval_buffer), "inval_buffer");
    EXPECT_EQ(fp::part_name(fp::Part::protect), "protect");
    EXPECT_EQ(fp::part_name(fp::Part::flip_wait), "flip_wait");
    EXPECT_EQ(fp::part_name(fp::Part::irq), "irq");
    EXPECT_EQ(fp::part_name(fp::Part::copy), "copy");
    EXPECT_EQ(fp::part_name(fp::Part::queue), "queue");
}

TEST_F(FrameProfile, LogFrameWritesNothingWhileOff) {
    fp::add(fp::Part::draw, 1);
    fp::log_frame();
    EXPECT_EQ(fp::take().calls[static_cast<std::size_t>(fp::Part::draw)], 1u);  // not cleared
}

}  // namespace
