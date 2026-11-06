#include "hle/kernel/time.hpp"

#include <pthread.h>
#include <sys/mman.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <utility>

#include <gtest/gtest.h>

#include "common/log.hpp"
#include "hle/hle.hpp"

namespace {

using namespace bb::hle::kernel;
using namespace std::chrono_literals;

constexpr int guest_eintr = 4;
constexpr int guest_einval = 22;

std::int64_t host_ns(clockid_t clock) {
    timespec t{};
    ::clock_gettime(clock, &t);
    return t.tv_sec * 1'000'000'000LL + t.tv_nsec;
}

std::int64_t ns(const GuestTimespec& t) {
    return t.tv_sec * 1'000'000'000LL + t.tv_nsec;
}

// How long `f` takes, in nanoseconds of CLOCK_MONOTONIC.
template <class F>
std::int64_t elapsed_ns(F f) {
    const std::int64_t start = host_ns(CLOCK_MONOTONIC);
    f();
    return host_ns(CLOCK_MONOTONIC) - start;
}

TEST(KernelTime, ClockIdsTranslate) {
    GuestTimespec t{};
    ASSERT_EQ(guest_clock_gettime(13, &t), 0);  // CLOCK_SECOND, libc's time()
    EXPECT_LT(std::abs(ns(t) - host_ns(CLOCK_REALTIME)), 1'000'000'000LL);
    ASSERT_EQ(guest_clock_gettime(4, &t), 0);  // CLOCK_MONOTONIC
    EXPECT_LT(std::abs(ns(t) - host_ns(CLOCK_MONOTONIC)), 1'000'000'000LL);
    ASSERT_EQ(clock_gettime_sce(4, &t), 0);  // QBi7HCK03hw, always clock 4 in the game
    EXPECT_LT(std::abs(ns(t) - host_ns(CLOCK_MONOTONIC)), 1'000'000'000LL);
    EXPECT_EQ(clock_gettime_sce(4, nullptr), sce_error(14));
}

TEST(KernelTime, UnknownClockIdsFail) {
    for (const std::int32_t id : {1, 3, 6, 16, -1}) {
        GuestTimespec t{};
        *guest_errno_location() = 0;
        EXPECT_EQ(guest_clock_gettime(id, &t), -1) << id;
        EXPECT_EQ(*guest_errno_location(), guest_einval) << id;
        EXPECT_EQ(clock_gettime_sce(id, &t), sce_error(guest_einval)) << id;
    }
}

// Logged as stub calls are: calls 1 to 10, then every 1000th. Once 10 calls are past (other tests
// may have made some), any 1000 calls log one line.
TEST(KernelTime, UnknownClockIdsAreLoggedRateLimited) {
    bb::common::set_log_filter(bb::common::LogFilter{});  // info and above
    GuestTimespec t{};
    for (int i = 0; i < 1010; ++i) {
        if (i == 10) {
            testing::internal::CaptureStderr();
        }
        guest_clock_gettime(-1, &t);
    }
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_EQ(log.find('\n'), log.size() - 1) << log;  // one line
    EXPECT_NE(log.find("unknown clock id -1"), std::string::npos) << log;
}

TEST(KernelTime, CountersNeverGoBackwards) {
    GuestTimespec last{};
    ASSERT_EQ(guest_clock_gettime(4, &last), 0);
    std::uint64_t usec = process_time_usec(), nsec = process_counter_nsec();
    for (int i = 0; i < 10000; ++i) {
        GuestTimespec t{};
        ASSERT_EQ(guest_clock_gettime(4, &t), 0);
        const std::uint64_t u = process_time_usec(), n = process_counter_nsec();
        ASSERT_GE(ns(t), ns(last));
        ASSERT_GE(u, usec);
        ASSERT_GE(n, nsec);
        last = t, usec = u, nsec = n;
    }
}

// 4J2sUJmuHZQ counts microseconds (libc's clock), -2IRUCO--PM nanoseconds.
TEST(KernelTime, CounterUnits) {
    const std::uint64_t usec = process_time_usec(), nsec = process_counter_nsec();
    std::this_thread::sleep_for(20ms);
    const std::uint64_t du = process_time_usec() - usec, dn = process_counter_nsec() - nsec;
    EXPECT_GE(du, 20'000u);
    EXPECT_LT(du, 2'000'000u);
    EXPECT_GE(dn, 20'000'000u);
    EXPECT_LT(dn, 2'000'000'000u);
    // Both count from the same origin.
    const std::uint64_t n = process_counter_nsec(), u = process_time_usec();
    EXPECT_LE(n / 1000, u);
    EXPECT_LT(u - n / 1000, 1'000'000u);
}

TEST(KernelTime, SleepsLastAtLeastTheRequestedTime) {
    const GuestTimespec request{0, 15'000'000};
    EXPECT_GE(elapsed_ns([&] { EXPECT_EQ(guest_nanosleep(&request, nullptr), 0); }), 15'000'000);
    EXPECT_GE(elapsed_ns([] { EXPECT_EQ(guest_usleep(15'000), 0); }), 15'000'000);
    EXPECT_GE(elapsed_ns([] { EXPECT_EQ(sceKernelUsleep(15'000), 0); }), 15'000'000);
    EXPECT_GE(elapsed_ns([] { EXPECT_EQ(guest_sleep(1), 0u); }), 1'000'000'000);
}

TEST(KernelTime, NanosleepRejectsInvalidRequests) {
    for (const GuestTimespec request : {GuestTimespec{0, -1}, GuestTimespec{0, 1'000'000'000},
                                        GuestTimespec{-1, 0}}) {
        *guest_errno_location() = 0;
        EXPECT_EQ(guest_nanosleep(&request, nullptr), -1);
        EXPECT_EQ(*guest_errno_location(), guest_einval);
    }
}

// Runs `f` on a thread and sends it SIGUSR1, with a handler and without SA_RESTART, until it
// returns. Returns its result and the guest errno it left on its thread.
template <class F>
auto interrupted(F f) {
    struct sigaction action {}, old{};
    action.sa_handler = [](int) {};
    sigemptyset(&action.sa_mask);
    sigaction(SIGUSR1, &action, &old);
    decltype(f()) result{};
    int error = 0;
    std::atomic<bool> done{false};
    std::thread thread([&] {
        *guest_errno_location() = 0;
        result = f();
        error = *guest_errno_location();
        done = true;
    });
    while (!done) {
        pthread_kill(thread.native_handle(), SIGUSR1);
        std::this_thread::sleep_for(5ms);
    }
    thread.join();
    sigaction(SIGUSR1, &old, nullptr);
    return std::pair{result, error};
}

TEST(KernelTime, InterruptedSleepsReportEintr) {
    GuestTimespec remain{};
    const GuestTimespec request{10, 0};
    auto [nano, nano_errno] = interrupted([&] { return guest_nanosleep(&request, &remain); });
    EXPECT_EQ(nano, -1);
    EXPECT_EQ(nano_errno, guest_eintr);
    EXPECT_GT(ns(remain), 0);
    EXPECT_LE(ns(remain), ns(request));

    auto [slept, usleep_errno] = interrupted([] { return guest_usleep(10'000'000); });
    EXPECT_EQ(slept, -1);
    EXPECT_EQ(usleep_errno, guest_eintr);

    auto [sce, sce_errno] = interrupted([] { return sceKernelUsleep(10'000'000); });
    EXPECT_EQ(sce, sce_error(guest_eintr));
    EXPECT_EQ(sce_errno, 0);  // sce calls leave errno alone

    auto [left, sleep_errno] = interrupted([] { return guest_sleep(10); });
    EXPECT_GE(left, 1u);
    EXPECT_LE(left, 10u);
    EXPECT_EQ(sleep_errno, 0);  // sleep reports the unslept seconds instead
}

TEST(KernelTime, WallClockAndTimezone) {
    const char* old = std::getenv("TZ");
    const std::string saved = old != nullptr ? old : "";
    setenv("TZ", "XST-5:30", 1);  // 5 h 30 min east of UTC, no daylight saving time
    tzset();

    GuestTimeval tv{};
    GuestTimezone tz{};
    ASSERT_EQ(guest_gettimeofday(&tv, &tz), 0);
    EXPECT_LT(std::abs(tv.tv_sec * 1'000'000'000LL + tv.tv_usec * 1000 - host_ns(CLOCK_REALTIME)),
              1'000'000'000LL);
    EXPECT_GE(tv.tv_usec, 0);
    EXPECT_LT(tv.tv_usec, 1'000'000);
    EXPECT_EQ(tz.minuteswest, -330);
    EXPECT_EQ(tz.dsttime, 0);
    // The eboot's bias in ms (0x1a3866): (minuteswest - (dsttime ? 60 : 0)) * -60000.
    EXPECT_EQ(tz.minuteswest * -60'000, 330 * 60'000);

    // kOcnerypnQA: libc adds both values and multiplies by 60 (libc.prx 0x1944).
    std::int32_t offsets[2] = {-1, -1};
    EXPECT_EQ(local_time_offsets(offsets), 0);
    EXPECT_EQ(offsets[0], 330);
    EXPECT_EQ(offsets[1], 0);

    // Daylight saving time all year: 1 h east in standard time, 2 h in daylight saving time.
    setenv("TZ", "XST-1XDT,0/0,J365/25", 1);
    tzset();
    ASSERT_EQ(guest_gettimeofday(nullptr, &tz), 0);
    EXPECT_EQ(tz.minuteswest, -60);
    EXPECT_EQ(tz.dsttime, 1);
    EXPECT_EQ(local_time_offsets(offsets), 0);
    EXPECT_EQ(offsets[0], 60);
    EXPECT_EQ(offsets[1], 60);

    tv = {};
    ASSERT_EQ(sceKernelGettimeofday(&tv), 0);
    EXPECT_GT(tv.tv_sec, 0);
    EXPECT_EQ(sceKernelGettimeofday(nullptr), sce_error(14));

    if (old != nullptr) {
        setenv("TZ", saved.c_str(), 1);
    } else {
        unsetenv("TZ");
    }
    tzset();
}

TEST(KernelTime, Getrusage) {
    GuestRusage usage{};
    std::memset(&usage, 0xff, sizeof usage);
    ASSERT_EQ(guest_getrusage(0, &usage), 0);
    EXPECT_GE(usage.utime.tv_sec, 0);
    EXPECT_GE(usage.utime.tv_usec, 0);
    EXPECT_LT(usage.utime.tv_usec, 1'000'000);
    EXPECT_GE(usage.stime.tv_sec, 0);
    EXPECT_GE(usage.stime.tv_usec, 0);
    EXPECT_LT(usage.stime.tv_usec, 1'000'000);
    for (const std::int64_t field : usage.other) {
        EXPECT_EQ(field, 0);
    }
    for (const std::int32_t who : {1, -1}) {
        *guest_errno_location() = 0;
        EXPECT_EQ(guest_getrusage(who, &usage), -1) << who;
        EXPECT_EQ(*guest_errno_location(), guest_einval) << who;
    }
}

std::atomic<std::uintptr_t> observed{0};
std::atomic<int> observed_calls{0};

void observe(std::uintptr_t return_address) {
    observed = return_address;
    ++observed_calls;
}

// A guest-style caller: `call` to a PLT entry that jumps through a GOT cell to the import.
//   0x00 sub rsp, 8 ; 0x04 call 0x10 ; 0x09 add rsp, 8 ; 0x0d ret
//   0x10 jmp [rip + 2] ; 0x18 the GOT cell
class SyntheticCaller {
public:
    explicit SyntheticCaller(std::uintptr_t target) {
        page_ = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        EXPECT_NE(page_, MAP_FAILED);
        const std::uint8_t code[] = {0x48, 0x83, 0xec, 0x08, 0xe8, 0x07, 0x00, 0x00,
                                     0x00, 0x48, 0x83, 0xc4, 0x08, 0xc3, 0xcc, 0xcc,
                                     0xff, 0x25, 0x02, 0x00, 0x00, 0x00, 0xcc, 0xcc};
        std::memcpy(page_, code, sizeof code);
        std::memcpy(static_cast<std::uint8_t*>(page_) + 0x18, &target, sizeof target);
        EXPECT_EQ(mprotect(page_, 4096, PROT_READ | PROT_EXEC), 0);
    }
    ~SyntheticCaller() { munmap(page_, 4096); }

    int operator()(GuestTimeval* tv, GuestTimezone* tz) const {
        return reinterpret_cast<int (*)(GuestTimeval*, GuestTimezone*)>(page_)(tv, tz);
    }
    std::uintptr_t return_site() const { return reinterpret_cast<std::uintptr_t>(page_) + 9; }

private:
    void* page_;
};

bb::loader::Import function_import(const char* library, const char* nid) {
    return {nid, library, "libkernel", 2 /* STT_FUNC */};
}

TEST(KernelTime, ObserverSeesTheCallersReturnAddress) {
    const std::uintptr_t posix = bb::hle::resolve(function_import("libScePosix", "n88vx3C5nW8"));
    const std::uintptr_t kernel = bb::hle::resolve(function_import("libkernel", "n88vx3C5nW8"));
    ASSERT_EQ(bb::hle::binding("libScePosix", "n88vx3C5nW8"), bb::hle::Binding::implemented);
    ASSERT_EQ(bb::hle::binding("libkernel", "n88vx3C5nW8"), bb::hle::Binding::implemented);

    observed = 0;
    observed_calls = 0;
    EXPECT_EQ(set_gettimeofday_observer(&observe), nullptr);
    const SyntheticCaller via_posix(posix);
    GuestTimeval tv{};
    EXPECT_EQ(via_posix(&tv, nullptr), 0);
    EXPECT_GT(tv.tv_sec, 0);
    EXPECT_EQ(observed_calls, 1);
    EXPECT_EQ(observed, via_posix.return_site());

    // libkernel's gettimeofday (libc.prx's) doesn't report.
    const SyntheticCaller via_kernel(kernel);
    EXPECT_EQ(via_kernel(&tv, nullptr), 0);
    EXPECT_EQ(observed_calls, 1);

    EXPECT_EQ(set_gettimeofday_observer(nullptr), &observe);
    EXPECT_EQ(via_posix(&tv, nullptr), 0);
    EXPECT_EQ(observed_calls, 1);
}

TEST(KernelTime, EveryTimeImportIsImplemented) {
    const std::pair<const char*, const char*> imports[] = {
        {"libkernel", "lLMT9vJAck0"}, {"libkernel", "yS8U2TGCe1A"}, {"libkernel", "hHlZQUnlxSM"},
        {"libkernel", "ejekcaNQNq0"}, {"libkernel", "1jfXLRVzisc"}, {"libkernel", "4J2sUJmuHZQ"},
        {"libkernel", "-2IRUCO--PM"}, {"libkernel", "QBi7HCK03hw"}, {"libkernel", "kOcnerypnQA"},
        {"libScePosix", "0wu33hunNdE"}, {"libScePosix", "QcteRwbsnV0"}};
    for (const auto& [library, nid] : imports) {
        bb::hle::resolve(function_import(library, nid));
        EXPECT_EQ(bb::hle::binding(library, nid), bb::hle::Binding::implemented) << nid;
    }
}

}  // namespace
