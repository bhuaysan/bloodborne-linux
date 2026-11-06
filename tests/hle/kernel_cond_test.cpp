// Condition variables, called through their registered NIDs, on guest mutexes.

#include <sys/time.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "hle/hle.hpp"
#include "hle/kernel/abi.hpp"
#include "loader/dynamic.hpp"

namespace {

using bb::hle::kernel::GuestTimespec;
using bb::hle::kernel::sce_error;
using Cell = std::uintptr_t;
using namespace std::chrono_literals;

template <class F>
F* fn(const char* library, const char* nid) {
    return reinterpret_cast<F*>(
        bb::hle::resolve({nid, library, "libTestModule", bb::loader::stt_func}));
}

constexpr const char* kernel = "libkernel";
constexpr const char* posix = "libScePosix";

auto* const sce_init = fn<std::int32_t(Cell*, const void*, const char*)>(kernel, "2Tb92quprl0");
auto* const sce_destroy = fn<std::int32_t(Cell*)>(kernel, "g+PZd2hiacg");
auto* const sce_wait = fn<std::int32_t(Cell*, Cell*)>(kernel, "WKAXJ4XBPQ4");
auto* const sce_timedwait = fn<std::int32_t(Cell*, Cell*, std::uint32_t)>(kernel, "BmMjYxmew1w");
auto* const sce_signal = fn<std::int32_t(Cell*)>(kernel, "kDh-NfxgMtE");
auto* const sce_broadcast = fn<std::int32_t(Cell*)>(kernel, "JGgj7Uvrl+A");

auto* const px_init = fn<int(Cell*, const void*)>(posix, "0TyVk4MSLt0");
auto* const px_destroy = fn<int(Cell*)>(posix, "RXXqi4CtF8w");
auto* const px_wait = fn<int(Cell*, Cell*)>(posix, "Op8TBGY5KHg");
auto* const px_timedwait = fn<int(Cell*, Cell*, const GuestTimespec*)>(posix, "27bAgiJmOh0");
auto* const px_signal = fn<int(Cell*)>(posix, "2MOy+rUfuhQ");
auto* const px_broadcast = fn<int(Cell*)>(posix, "mkx2fVhNMsg");

auto* const mutex_lock = fn<int(Cell*)>(posix, "7H0iTOciTLo");
auto* const mutex_unlock = fn<int(Cell*)>(posix, "2Z+PpY6CaJg");
auto* const mutex_destroy = fn<int(Cell*)>(posix, "ltCfaGr2JGE");

constexpr int eperm = 1, ebusy = 16, einval = 22, etimedout = 60;

// A condition variable with `n` waiter threads, each counting its wakeups. Mutex and condition
// variable start as static initialisers.
struct Waiters {
    Cell mutex = 0;
    Cell cond = 0;
    int ready = 0;     // waiters that hold the mutex and are about to wait
    int released = 0;  // wakeups the test has allowed
    std::atomic<int> woken{0};
    std::vector<std::thread> threads;

    explicit Waiters(int n) {
        for (int i = 0; i < n; ++i) {
            threads.emplace_back([this] {
                mutex_lock(&mutex);
                ++ready;
                while (released == 0) {
                    EXPECT_EQ(sce_wait(&cond, &mutex), 0);
                }
                --released;
                ++woken;
                mutex_unlock(&mutex);
            });
        }
        // Every waiter is inside its wait once it has released the mutex with ready counted.
        for (;;) {
            mutex_lock(&mutex);
            const bool all = ready == n;
            mutex_unlock(&mutex);
            if (all) {
                break;
            }
            std::this_thread::sleep_for(1ms);
        }
    }

    void release(int n, bool all) {
        mutex_lock(&mutex);
        released += n;
        EXPECT_EQ(all ? sce_broadcast(&cond) : sce_signal(&cond), 0);
        mutex_unlock(&mutex);
    }

    ~Waiters() {
        release(static_cast<int>(threads.size()), true);
        for (auto& t : threads) {
            t.join();
        }
        EXPECT_EQ(sce_destroy(&cond), 0);
        EXPECT_EQ(mutex_destroy(&mutex), 0);
    }
};

TEST(KernelCond, SignalWakesOneWaiterAndBroadcastWakesAll) {
    Waiters w(3);
    w.release(1, false);
    while (w.woken < 1) {
        std::this_thread::sleep_for(1ms);
    }
    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(w.woken, 1);

    w.release(2, true);
    while (w.woken < 3) {
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_EQ(w.woken, 3);
}

TEST(KernelCond, SceTimedwaitTimesOutAfterItsMicroseconds) {
    Cell mutex = 0, cond = 0;
    ASSERT_EQ(sce_init(&cond, nullptr, "test"), 0);
    mutex_lock(&mutex);
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(sce_timedwait(&cond, &mutex, 30000), sce_error(etimedout));
    EXPECT_GE(std::chrono::steady_clock::now() - start, 30ms);
    // The mutex is held again after the timeout.
    EXPECT_EQ(mutex_unlock(&mutex), 0);
    EXPECT_EQ(sce_destroy(&cond), 0);
    EXPECT_EQ(mutex_destroy(&mutex), 0);
}

TEST(KernelCond, PosixTimedwaitTakesAnAbsoluteWallClockTime) {
    Cell mutex = 0, cond = 0;
    ASSERT_EQ(px_init(&cond, nullptr), 0);
    timeval now{};
    gettimeofday(&now, nullptr);
    GuestTimespec deadline{now.tv_sec, (now.tv_usec + 30000) * 1000};
    if (deadline.tv_nsec >= 1000000000) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000;
    }
    mutex_lock(&mutex);
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(px_timedwait(&cond, &mutex, &deadline), etimedout);
    EXPECT_GE(std::chrono::steady_clock::now() - start, 25ms);
    // A time in the past times out at once; a bad one is refused.
    deadline.tv_sec -= 10;
    EXPECT_EQ(px_timedwait(&cond, &mutex, &deadline), etimedout);
    deadline.tv_nsec = 1000000000;
    EXPECT_EQ(px_timedwait(&cond, &mutex, &deadline), einval);
    EXPECT_EQ(px_timedwait(&cond, &mutex, nullptr), einval);
    mutex_unlock(&mutex);
    EXPECT_EQ(px_destroy(&cond), 0);
    EXPECT_EQ(mutex_destroy(&mutex), 0);
}

TEST(KernelCond, PosixSignalWakesATimedWaiter) {
    Cell mutex = 0, cond = 0;
    bool flag = false;
    std::thread signaller([&] {
        std::this_thread::sleep_for(10ms);
        mutex_lock(&mutex);
        flag = true;
        EXPECT_EQ(px_signal(&cond), 0);
        mutex_unlock(&mutex);
    });
    timeval now{};
    gettimeofday(&now, nullptr);
    const GuestTimespec deadline{now.tv_sec + 10, now.tv_usec * 1000};
    mutex_lock(&mutex);
    int result = 0;
    while (!flag && result == 0) {
        result = px_timedwait(&cond, &mutex, &deadline);
    }
    EXPECT_EQ(result, 0);
    mutex_unlock(&mutex);
    signaller.join();
    EXPECT_EQ(px_broadcast(&cond), 0);
    EXPECT_EQ(px_destroy(&cond), 0);
    EXPECT_EQ(mutex_destroy(&mutex), 0);
}

TEST(KernelCond, WaitWithoutTheMutexHeldIsRefused) {
    Cell mutex = 0, cond = 0;
    // The default mutex type is error-checking, so the wait cannot release a mutex it doesn't
    // hold. The eboot compares 0x80020001 (0x2083ed1).
    EXPECT_EQ(sce_wait(&cond, &mutex), sce_error(eperm));
    EXPECT_EQ(px_wait(&cond, &mutex), eperm);
    EXPECT_EQ(sce_destroy(&cond), 0);
    EXPECT_EQ(mutex_destroy(&mutex), 0);
}

TEST(KernelCond, DestroyWithAWaiterIsBusyAndGarbageIsRefused) {
    Cell mutex = 0, cond = 0;
    bool go = false;
    std::atomic<bool> waiting{false};
    std::thread waiter([&] {
        mutex_lock(&mutex);
        waiting = true;
        while (!go) {
            sce_wait(&cond, &mutex);
        }
        mutex_unlock(&mutex);
    });
    while (!waiting) {
        std::this_thread::sleep_for(1ms);
    }
    mutex_lock(&mutex);  // the waiter is inside its wait now
    EXPECT_EQ(sce_destroy(&cond), sce_error(ebusy));
    EXPECT_EQ(px_destroy(&cond), ebusy);
    go = true;
    sce_broadcast(&cond);
    mutex_unlock(&mutex);
    waiter.join();
    EXPECT_EQ(sce_destroy(&cond), 0);
    EXPECT_EQ(cond, 0u);
    EXPECT_EQ(mutex_destroy(&mutex), 0);

    Cell garbage = 0x1234;
    EXPECT_EQ(sce_signal(&garbage), sce_error(einval));
    EXPECT_EQ(px_wait(&garbage, &mutex), einval);
    EXPECT_EQ(sce_init(nullptr, nullptr, nullptr), sce_error(einval));
    EXPECT_EQ(px_signal(nullptr), einval);
}

}  // namespace
