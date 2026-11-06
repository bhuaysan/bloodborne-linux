// libSceRtc: dates and microsecond ticks. The facts come from the eboot's call sites;
// docs/formats/rtc.md has them with the addresses that show them. Function names are the ones
// in symbols/nids.csv.
#pragma once

#include <cstdint>

namespace bb::hle::system {

// The 16-byte date the game passes (eboot 0x20ee3c5-0x20ee3e0 fills it, 0x20ee23d-0x20ee302
// reads it). Month and day count from 1.
struct GuestDate {
    std::uint16_t year;
    std::uint16_t month;
    std::uint16_t day;
    std::uint16_t hour;
    std::uint16_t minute;
    std::uint16_t second;
    std::uint32_t microsecond;
};
static_assert(sizeof(GuestDate) == 16);

// A tick counts microseconds (eboot 0x20ee420-0x20ee483). The code doesn't show its epoch, so
// it is our choice: 1600-01-01 00:00:00 UTC, the start of the 400-year calendar cycle that holds
// the lowest year the game accepts (1601, eboot 0x20ee240). Every date the game handles then
// has a positive tick, and 0 stays free for "no date" (eboot 0x145b77e).
inline constexpr std::uint64_t ticks_per_second_value = 1'000'000;

// The current date and time in UTC (sceRtcGetCurrentClockLocalTime, ZPD1YOKI+Kw).
std::int32_t date_now(GuestDate* out);
// A date as a tick (sceRtcGetTick, 8w-H19ip48I). Fails for an invalid date.
std::int32_t date_to_tick(const GuestDate* date, std::uint64_t* tick);
// A tick as a date (sceRtcSetTick, ueega6v3GUw). Fails past the year 9999.
std::int32_t tick_to_date(GuestDate* out, const std::uint64_t* tick);
// An integer from a text header as a date (sceRtcSetTime_t, bDEVVP4bTjQ): our choice, seconds
// since 1970-01-01 00:00:00 UTC. Its only caller parses a network header (eboot 0xcb6fd7).
std::int32_t seconds_to_date(GuestDate* out, std::uint64_t seconds);
// The current time as a tick (sceRtcGetCurrentNetworkTick, zO9UL3qIINQ).
std::int32_t tick_now(std::uint64_t* tick);
// Ticks per second (sceRtcGetTickResolution, jMNwqYr4R-k); the caller divides tick deltas by it.
std::int32_t ticks_per_second();
// The two tick conversions the game switches with bit 51 of its own date object:
// sceRtcConvertUtcToLocalTime (M1TvFst-jrM) clears the bit, sceRtcConvertLocalTimeToUtc
// (8Yr143yEnRo) sets it. The runtime keeps one time zone, UTC, so both copy the tick.
std::int32_t tick_convert_clearing_flag(const std::uint64_t* in, std::uint64_t* out);
std::int32_t tick_convert_setting_flag(const std::uint64_t* in, std::uint64_t* out);
// The day of the week of a date (sceRtcGetDayOfWeek, CyIK-i4XdgQ): 0 for Sunday to 6 for Saturday,
// our choice, as in C's tm_wday.
std::int32_t day_of_week(std::int32_t year, std::int32_t month, std::int32_t day);
// Text for a signed upload request (sceRtcFormatRFC2822LocalTime, AxHBk3eat04). The second
// argument is a tick pointer, which the game passes as null. Always fails, so the game uses its
// empty default (eboot 0x1fe5ca9): the network is offline (#91).
std::int32_t date_text(char* buffer, const std::uint64_t* tick);

}  // namespace bb::hle::system
