// libSceRtc: dates and microsecond ticks (date_time.hpp, docs/formats/rtc.md).

#include "hle/system/date_time.hpp"

#include <chrono>
#include <cstdint>
#include <ctime>

#include "hle/hle.hpp"
#include "hle/kernel/abi.hpp"

namespace bb::hle::system {
namespace {

using namespace std::chrono;

constexpr std::int32_t invalid = kernel::sce_error(22);  // guest EINVAL

// The tick epoch, 1600-01-01 00:00:00 UTC, and the end of what a date can hold.
constexpr sys_days epoch = sys_days{year{1600} / January / 1};
constexpr sys_days end = sys_days{year{10000} / January / 1};

constexpr std::uint64_t to_ticks(microseconds since_epoch) {
    return static_cast<std::uint64_t>(since_epoch.count());
}

// The date of a point in time at or after the epoch and before the year 10000.
GuestDate date_of(sys_time<microseconds> t) {
    const sys_days day = floor<days>(t);
    const year_month_day ymd{day};
    const hh_mm_ss<microseconds> time{t - day};
    return GuestDate{
        .year = static_cast<std::uint16_t>(static_cast<int>(ymd.year())),
        .month = static_cast<std::uint16_t>(static_cast<unsigned>(ymd.month())),
        .day = static_cast<std::uint16_t>(static_cast<unsigned>(ymd.day())),
        .hour = static_cast<std::uint16_t>(time.hours().count()),
        .minute = static_cast<std::uint16_t>(time.minutes().count()),
        .second = static_cast<std::uint16_t>(time.seconds().count()),
        .microsecond = static_cast<std::uint32_t>(time.subseconds().count()),
    };
}

std::int32_t store_date(GuestDate* out, sys_time<microseconds> t) {
    if (out == nullptr || t < epoch || t >= end) {
        return invalid;
    }
    *out = date_of(t);
    return 0;
}

}  // namespace

std::int32_t date_now(GuestDate* out) {
    return store_date(out, floor<microseconds>(system_clock::now()));
}

std::int32_t date_to_tick(const GuestDate* date, std::uint64_t* tick) {
    // chrono's month and day keep 8 bits, so wider values are refused before they wrap.
    if (date == nullptr || tick == nullptr || date->month < 1 || date->month > 12 ||
        date->day < 1 || date->day > 31) {
        return invalid;
    }
    const year_month_day ymd{year{date->year}, month{date->month}, day{date->day}};
    if (!ymd.ok() || sys_days{ymd} < epoch || sys_days{ymd} >= end || date->hour > 23 ||
        date->minute > 59 || date->second > 59 || date->microsecond > 999'999) {
        return invalid;
    }
    const sys_time<microseconds> t = sys_days{ymd} + hours{date->hour} + minutes{date->minute} +
                                     seconds{date->second} + microseconds{date->microsecond};
    *tick = to_ticks(t - epoch);
    return 0;
}

std::int32_t tick_to_date(GuestDate* out, const std::uint64_t* tick) {
    if (tick == nullptr || *tick >= to_ticks(end - epoch)) {
        return invalid;
    }
    return store_date(out, epoch + microseconds{static_cast<std::int64_t>(*tick)});
}

std::int32_t seconds_to_date(GuestDate* out, std::uint64_t value) {
    if (value >= static_cast<std::uint64_t>(duration_cast<seconds>(end - sys_days{}).count())) {
        return invalid;
    }
    return store_date(out, sys_seconds{seconds{static_cast<std::int64_t>(value)}});
}

std::int32_t tick_now(std::uint64_t* tick) {
    if (tick == nullptr) {
        return invalid;
    }
    *tick = to_ticks(floor<microseconds>(system_clock::now()) - epoch);
    return 0;
}

std::int32_t ticks_per_second() {
    return static_cast<std::int32_t>(ticks_per_second_value);
}

std::int32_t tick_convert_clearing_flag(const std::uint64_t* in, std::uint64_t* out) {
    if (in == nullptr || out == nullptr) {
        return invalid;
    }
    *out = *in;
    return 0;
}

std::int32_t tick_convert_setting_flag(const std::uint64_t* in, std::uint64_t* out) {
    return tick_convert_clearing_flag(in, out);
}

std::int32_t day_of_week(std::int32_t y, std::int32_t m, std::int32_t d) {
    if (y < 1 || y > 9999 || m < 1 || m > 12 || d < 1 || d > 31) {
        return invalid;
    }
    const year_month_day ymd{year{y}, month{static_cast<unsigned>(m)},
                             day{static_cast<unsigned>(d)}};
    if (!ymd.ok()) {
        return invalid;
    }
    return static_cast<std::int32_t>(weekday{sys_days{ymd}}.c_encoding());
}

std::int32_t date_text(char* /*buffer*/, std::int32_t /*mode*/) {
    return invalid;
}

BB_HLE_FUNCTION("libSceRtc", "ZPD1YOKI+Kw", date_now);                     // name unknown
BB_HLE_FUNCTION("libSceRtc", "8w-H19ip48I", date_to_tick);                 // name unknown
BB_HLE_FUNCTION("libSceRtc", "ueega6v3GUw", tick_to_date);                 // name unknown
BB_HLE_FUNCTION("libSceRtc", "bDEVVP4bTjQ", seconds_to_date);              // name unknown
BB_HLE_FUNCTION("libSceRtc", "zO9UL3qIINQ", tick_now);                     // name unknown
BB_HLE_FUNCTION("libSceRtc", "jMNwqYr4R-k", ticks_per_second);             // name unknown
BB_HLE_FUNCTION("libSceRtc", "M1TvFst-jrM", tick_convert_clearing_flag);   // name unknown
BB_HLE_FUNCTION("libSceRtc", "8Yr143yEnRo", tick_convert_setting_flag);    // name unknown
BB_HLE_FUNCTION("libSceRtc", "CyIK-i4XdgQ", day_of_week);                  // name unknown
BB_HLE_FUNCTION("libSceRtc", "AxHBk3eat04", date_text);                    // name unknown

}  // namespace bb::hle::system
