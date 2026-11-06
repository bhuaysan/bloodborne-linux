#include "hle/system/date_time.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>

#include <gtest/gtest.h>

#include "hle/hle.hpp"
#include "loader/dynamic.hpp"

namespace {

using namespace bb::hle::system;

constexpr std::int32_t invalid = static_cast<std::int32_t>(0x80020016u);

bool same(const GuestDate& a, const GuestDate& b) {
    return std::memcmp(&a, &b, sizeof a) == 0;
}

// Ticks of known dates: microseconds since 1600-01-01 00:00:00 UTC, computed independently with
// Python's datetime (proleptic Gregorian calendar).
struct Known {
    GuestDate date;
    std::uint64_t tick;
    std::int32_t weekday;  // 0 = Sunday
};
constexpr Known known[] = {
    {{1600, 1, 1, 0, 0, 0, 0}, 0, 6},
    {{1601, 1, 1, 0, 0, 0, 0}, 31'622'400'000'000, 1},
    {{1970, 1, 1, 0, 0, 0, 0}, 11'676'096'000'000'000, 4},
    {{2000, 2, 29, 23, 59, 59, 999'999}, 12'627'964'799'999'999, 2},
    {{2015, 3, 24, 12, 34, 56, 789'012}, 13'103'296'496'789'012, 2},
    {{9999, 12, 31, 23, 59, 59, 999'999}, 265'078'396'799'999'999, 5},
};

TEST(DateTime, KnownDatesGiveTheirTicks) {
    for (const Known& k : known) {
        std::uint64_t tick = 1;
        EXPECT_EQ(date_to_tick(&k.date, &tick), 0) << k.date.year;
        EXPECT_EQ(tick, k.tick) << k.date.year;
        GuestDate back{};
        EXPECT_EQ(tick_to_date(&back, &k.tick), 0) << k.date.year;
        EXPECT_TRUE(same(back, k.date)) << k.date.year;
    }
}

// The game converts a date to a tick and back and compares the 16 bytes (eboot 0x20ee4f5).
TEST(DateTime, ConversionsRoundTrip) {
    for (std::uint64_t tick = 1; tick < 265'078'396'800'000'000; tick = tick * 3 + 12'345) {
        GuestDate date{};
        ASSERT_EQ(tick_to_date(&date, &tick), 0) << tick;
        std::uint64_t back = 0;
        ASSERT_EQ(date_to_tick(&date, &back), 0) << tick;
        EXPECT_EQ(back, tick);
    }
}

TEST(DateTime, InvalidDatesAndTicksFail) {
    std::uint64_t tick = 0;
    for (const GuestDate& bad : {GuestDate{2015, 2, 29, 0, 0, 0, 0}, GuestDate{2015, 13, 1, 0, 0, 0, 0},
                                 GuestDate{2015, 1, 0, 0, 0, 0, 0}, GuestDate{2015, 1, 1, 24, 0, 0, 0},
                                 GuestDate{2015, 1, 1, 0, 60, 0, 0}, GuestDate{2015, 1, 1, 0, 0, 60, 0},
                                 GuestDate{2015, 1, 1, 0, 0, 0, 1'000'000},
                                 GuestDate{1599, 12, 31, 0, 0, 0, 0},
                                 GuestDate{10000, 1, 1, 0, 0, 0, 0},
                                 GuestDate{2015, 257, 1, 0, 0, 0, 0},
                                 GuestDate{2015, 1, 257, 0, 0, 0, 0}}) {
        EXPECT_EQ(date_to_tick(&bad, &tick), invalid) << bad.year << "-" << bad.month;
    }
    GuestDate date{};
    const std::uint64_t past_end = 265'078'396'800'000'000;
    EXPECT_EQ(tick_to_date(&date, &past_end), invalid);
    EXPECT_EQ(date_to_tick(nullptr, &tick), invalid);
    EXPECT_EQ(tick_to_date(nullptr, &past_end), invalid);
}

TEST(DateTime, NowAgreesBetweenTheDateAndTheTick) {
    // The game subtracts the current tick from a date's tick (eboot 0xcb70d8), so both share one
    // scale and origin.
    GuestDate date{};
    std::uint64_t from_date = 0;
    std::uint64_t now = 0;
    ASSERT_EQ(date_now(&date), 0);
    ASSERT_EQ(date_to_tick(&date, &from_date), 0);
    ASSERT_EQ(tick_now(&now), 0);
    EXPECT_GE(now, from_date);
    EXPECT_LT(now - from_date, 5 * ticks_per_second_value);
    EXPECT_GE(date.year, 2026);
    EXPECT_EQ(ticks_per_second(), 1'000'000);
}

TEST(DateTime, SecondsSinceTheUnixEpoch) {
    GuestDate date{};
    ASSERT_EQ(seconds_to_date(&date, 1'427'200'496), 0);
    EXPECT_TRUE(same(date, GuestDate{2015, 3, 24, 12, 34, 56, 0}));
    EXPECT_EQ(seconds_to_date(&date, 0), 0);
    EXPECT_TRUE(same(date, GuestDate{1970, 1, 1, 0, 0, 0, 0}));
    EXPECT_EQ(seconds_to_date(&date, ~std::uint64_t{0}), invalid);
}

TEST(DateTime, DayOfWeek) {
    for (const Known& k : known) {
        EXPECT_EQ(day_of_week(k.date.year, k.date.month, k.date.day), k.weekday) << k.date.year;
    }
    EXPECT_EQ(day_of_week(2015, 2, 29), invalid);
    EXPECT_EQ(day_of_week(2015, 0, 1), invalid);
    EXPECT_EQ(day_of_week(40000, 1, 1), invalid);
}

TEST(DateTime, TickConversionsAndTextKeepTheGameOnItsDefaults) {
    const std::uint64_t in = 13'103'296'496'789'012;
    std::uint64_t out = 0;
    EXPECT_EQ(tick_convert_clearing_flag(&in, &out), 0);
    EXPECT_EQ(out, in);
    out = 0;
    EXPECT_EQ(tick_convert_setting_flag(&in, &out), 0);
    EXPECT_EQ(out, in);
    char text[8] = "keep";
    EXPECT_NE(date_text(text, 0), 0);
    EXPECT_STREQ(text, "keep");
}

TEST(DateTime, NidsResolveToTheImplementations) {
    struct Entry {
        const char* nid;
        void* impl;
    };
    const Entry entries[] = {
        {"ZPD1YOKI+Kw", reinterpret_cast<void*>(&date_now)},
        {"8w-H19ip48I", reinterpret_cast<void*>(&date_to_tick)},
        {"ueega6v3GUw", reinterpret_cast<void*>(&tick_to_date)},
        {"bDEVVP4bTjQ", reinterpret_cast<void*>(&seconds_to_date)},
        {"zO9UL3qIINQ", reinterpret_cast<void*>(&tick_now)},
        {"jMNwqYr4R-k", reinterpret_cast<void*>(&ticks_per_second)},
        {"M1TvFst-jrM", reinterpret_cast<void*>(&tick_convert_clearing_flag)},
        {"8Yr143yEnRo", reinterpret_cast<void*>(&tick_convert_setting_flag)},
        {"CyIK-i4XdgQ", reinterpret_cast<void*>(&day_of_week)},
        {"AxHBk3eat04", reinterpret_cast<void*>(&date_text)},
    };
    for (const Entry& e : entries) {
        EXPECT_EQ(bb::hle::resolve({e.nid, "libSceRtc", "libSceRtc", bb::loader::stt_func}),
                  reinterpret_cast<std::uintptr_t>(e.impl))
            << e.nid;
    }
}

}  // namespace
