#include "hle/kernel/time.hpp"

#include <sys/resource.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <ctime>
#include <string_view>

#include "common/log.hpp"
#include "hle/hle.hpp"

namespace bb::hle::kernel {
namespace {

static_assert(sizeof(GuestTimezone) == 8);
// libc 0x2eef0 reserves 0x90 bytes for it and reads the two timevals at 0 and 16.
static_assert(sizeof(GuestRusage) == 144 && offsetof(GuestRusage, stime) == 16);

std::atomic<GettimeofdayObserver> observer{nullptr};

// Logs an unknown clock id on calls 1 to 10 of `call`, then every 1000th, as for stubs. The
// count is per call site, not per clock id.
void log_unknown_clock(std::atomic<std::uint64_t>& calls, std::string_view call,
                       std::int32_t clock) {
    if (should_log(++calls)) {
        common::log(common::LogLevel::warn, "hle.libkernel", "{}: unknown clock id {}", call,
                    clock);
    }
}

timespec now(clockid_t clock) {
    timespec t{};
    ::clock_gettime(clock, &t);
    return t;
}

std::uint64_t nanoseconds(const timespec& t) {
    return static_cast<std::uint64_t>(t.tv_sec) * 1'000'000'000u +
           static_cast<std::uint64_t>(t.tv_nsec);
}

// The origin of the process counters: static initialisation, before the game runs.
const std::uint64_t start_nsec = nanoseconds(now(CLOCK_MONOTONIC));

std::uint64_t since_start_nsec() {
    return nanoseconds(now(CLOCK_MONOTONIC)) - start_nsec;
}

bool valid(const GuestTimespec& t) {
    return t.tv_sec >= 0 && t.tv_nsec >= 0 && t.tv_nsec < 1'000'000'000;
}

// Sleeps for `request` on CLOCK_MONOTONIC. Returns 0 or the host errno; on EINTR `remain` holds
// the time left.
int sleep_for(const timespec& request, timespec* remain) {
    return clock_nanosleep(CLOCK_MONOTONIC, 0, &request, remain);
}

timespec from_microseconds(std::uint32_t microseconds) {
    return {static_cast<time_t>(microseconds / 1'000'000),
            static_cast<long>(microseconds % 1'000'000) * 1000};
}

// The host's offset east of UTC, in minutes, now: in standard time, and the daylight saving
// shift (60 or 0). The game adds both and assumes a one-hour shift (libc.prx 0x1944,
// eboot 0x1a386d).
struct LocalOffset {
    std::int32_t standard, dst;
};

LocalOffset local_offset() {
    const time_t t = ::time(nullptr);
    tm local{};
    localtime_r(&t, &local);
    const std::int32_t dst = local.tm_isdst > 0 ? 60 : 0;
    return {static_cast<std::int32_t>(local.tm_gmtoff / 60) - dst, dst};
}

int gettimeofday_impl(GuestTimeval* tv, GuestTimezone* tz) {
    if (tv != nullptr) {
        const timespec t = now(CLOCK_REALTIME);
        *tv = {t.tv_sec, t.tv_nsec / 1000};
    }
    if (tz != nullptr) {
        const LocalOffset offset = local_offset();
        *tz = {-offset.standard, offset.dst != 0 ? 1 : 0};
    }
    return 0;
}

}  // namespace

GettimeofdayObserver set_gettimeofday_observer(GettimeofdayObserver next) {
    return observer.exchange(next, std::memory_order_acq_rel);
}

int guest_clock_gettime(std::int32_t clock, GuestTimespec* out) {
    const auto host = host_clock(clock);
    if (!host) {
        static std::atomic<std::uint64_t> calls{0};
        log_unknown_clock(calls, "clock_gettime", clock);
        return posix_fail(EINVAL);
    }
    if (out == nullptr) {
        return posix_fail(EFAULT);
    }
    *out = guest_timespec(now(*host));
    return 0;
}

int guest_gettimeofday(GuestTimeval* tv, GuestTimezone* tz) {
    return gettimeofday_impl(tv, tz);
}

// Not inlined, so that its return address is the guest's call site.
[[gnu::noinline]] int posix_gettimeofday(GuestTimeval* tv, GuestTimezone* tz) {
    if (const GettimeofdayObserver o = observer.load(std::memory_order_acquire)) {
        o(reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)));
    }
    return gettimeofday_impl(tv, tz);
}

int guest_nanosleep(const GuestTimespec* request, GuestTimespec* remain) {
    if (request == nullptr) {
        return posix_fail(EFAULT);
    }
    if (!valid(*request)) {
        return posix_fail(EINVAL);
    }
    timespec left{};
    const int error = sleep_for(host_timespec(*request), &left);
    if (error == EINTR && remain != nullptr) {
        *remain = guest_timespec(left);
    }
    return error == 0 ? 0 : posix_fail(error);
}

int guest_usleep(std::uint32_t microseconds) {
    const int error = sleep_for(from_microseconds(microseconds), nullptr);
    return error == 0 ? 0 : posix_fail(error);
}

// Unslept time is rounded up to whole seconds, so an interrupted sleep never reports 0.
std::uint32_t guest_sleep(std::uint32_t seconds) {
    timespec left{};
    const int error = sleep_for({static_cast<time_t>(seconds), 0}, &left);
    if (error != EINTR) {
        return 0;
    }
    return static_cast<std::uint32_t>(left.tv_sec) + (left.tv_nsec != 0 ? 1 : 0);
}

int guest_getrusage(std::int32_t who, GuestRusage* out) {
    if (who != 0) {  // RUSAGE_SELF, the only value libc passes (libc 0x2ef11)
        return posix_fail(EINVAL);
    }
    if (out == nullptr) {
        return posix_fail(EFAULT);
    }
    rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) != 0) {
        return posix_fail(errno);
    }
    *out = {guest_timeval(usage.ru_utime), guest_timeval(usage.ru_stime), {}};
    return 0;
}

std::int32_t sceKernelGettimeofday(GuestTimeval* tv) {
    if (tv == nullptr) {
        return sce_fail(EFAULT);
    }
    return gettimeofday_impl(tv, nullptr);
}

std::int32_t sceKernelUsleep(std::uint32_t microseconds) {
    return sce_fail(sleep_for(from_microseconds(microseconds), nullptr));
}

std::uint64_t process_time_usec() {
    return since_start_nsec() / 1000;
}

std::uint64_t process_counter_nsec() {
    return since_start_nsec();
}

std::int32_t clock_gettime_sce(std::int32_t clock, GuestTimespec* out) {
    const auto host = host_clock(clock);
    if (!host) {
        static std::atomic<std::uint64_t> calls{0};
        log_unknown_clock(calls, "QBi7HCK03hw", clock);
        return sce_fail(EINVAL);
    }
    if (out == nullptr) {
        return sce_fail(EFAULT);
    }
    *out = guest_timespec(now(*host));
    return 0;
}

std::int32_t local_time_offsets(std::int32_t out[2]) {
    const LocalOffset offset = local_offset();
    out[0] = offset.standard;
    out[1] = offset.dst;
    return 0;
}

}  // namespace bb::hle::kernel

namespace bk = bb::hle::kernel;

BB_HLE_FUNCTION("libkernel", "lLMT9vJAck0", bk::guest_clock_gettime);
BB_HLE_FUNCTION("libkernel", "n88vx3C5nW8", bk::guest_gettimeofday);
BB_HLE_FUNCTION("libkernel", "yS8U2TGCe1A", bk::guest_nanosleep);
BB_HLE_FUNCTION("libkernel", "hHlZQUnlxSM", bk::guest_getrusage);
BB_HLE_FUNCTION("libkernel", "ejekcaNQNq0", bk::sceKernelGettimeofday);
BB_HLE_FUNCTION("libkernel", "1jfXLRVzisc", bk::sceKernelUsleep);
BB_HLE_FUNCTION("libkernel", "4J2sUJmuHZQ", bk::process_time_usec);    // sceKernelGetProcessTime
BB_HLE_FUNCTION("libkernel", "-2IRUCO--PM", bk::process_counter_nsec);  // sceKernelReadTsc
BB_HLE_FUNCTION("libkernel", "QBi7HCK03hw", bk::clock_gettime_sce);     // sceKernelClockGettime
BB_HLE_FUNCTION("libkernel", "kOcnerypnQA", bk::local_time_offsets);    // sceKernelGettimezone
BB_HLE_FUNCTION("libScePosix", "n88vx3C5nW8", bk::posix_gettimeofday);
BB_HLE_FUNCTION("libScePosix", "0wu33hunNdE", bk::guest_sleep);
BB_HLE_FUNCTION("libScePosix", "QcteRwbsnV0", bk::guest_usleep);
