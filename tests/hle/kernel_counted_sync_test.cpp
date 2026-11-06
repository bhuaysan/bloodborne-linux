// The counted sync object, called through its registered NIDs (names unknown).

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "hle/hle.hpp"
#include "hle/kernel/abi.hpp"
#include "loader/dynamic.hpp"
#include "support/hle.hpp"

namespace {

using bb::hle::kernel::sce_error;
using namespace std::chrono_literals;

using bb::test::fn;

auto* const create = fn<std::int32_t(std::uint64_t*, const char*, std::int32_t, std::int32_t,
                                     std::int32_t, const void*)>("188x57JYp0g");
auto* const destroy = fn<std::int32_t(std::uint64_t)>("R1Jvn8bSCW8");
auto* const wait = fn<std::int32_t(std::uint64_t, std::int32_t, const std::uint32_t*)>(
    "Zxa0VhQVTsk");
auto* const try_wait = fn<std::int32_t(std::uint64_t, std::int32_t)>("12wOHk8ywb0");
auto* const post = fn<std::int32_t(std::uint64_t, std::int32_t)>("4czppHBiriw");

constexpr int esrch = 3, eacces = 13, ebusy = 16, einval = 22, etimedout = 60,
              enametoolong = 63;

std::uint64_t make(std::int32_t initial, std::int32_t max, std::int32_t attribute = 1) {
    std::uint64_t handle = 0;
    EXPECT_EQ(create(&handle, "", attribute, initial, max, nullptr), 0);
    EXPECT_NE(handle, 0u);
    return handle;
}

TEST(KernelCountedSync, CreateChecksItsArguments) {
    std::uint64_t h = 0;
    EXPECT_EQ(create(nullptr, "", 1, 0, 1, nullptr), sce_error(einval));
    EXPECT_EQ(create(&h, "", 3, 0, 1, nullptr), sce_error(einval));
    EXPECT_EQ(create(&h, "", 1, 2, 1, nullptr), sce_error(einval));
    EXPECT_EQ(create(&h, "", 1, -1, 1, nullptr), sce_error(einval));
    EXPECT_EQ(create(&h, "", 1, 0, 0, nullptr), sce_error(einval));
    EXPECT_EQ(create(&h, "a name that is longer than 31 bytes", 1, 0, 1, nullptr),
              sce_error(enametoolong));
    EXPECT_EQ(h, 0u);

    const std::uint64_t a = make(0, 1, 2);
    const std::uint64_t b = make(0, 0xffff);
    EXPECT_NE(a, b);
    EXPECT_EQ(destroy(a), 0);
    EXPECT_EQ(destroy(a), sce_error(esrch));
    EXPECT_EQ(wait(a, 1, nullptr), sce_error(esrch));
    EXPECT_EQ(post(a, 1), sce_error(esrch));
    EXPECT_EQ(try_wait(a, 1), sce_error(esrch));
    EXPECT_EQ(destroy(b), 0);
}

TEST(KernelCountedSync, CountRespectsItsMaximum) {
    const std::uint64_t h = make(1, 3);
    EXPECT_EQ(post(h, 2), 0);
    // A post past the maximum is refused and changes nothing (the eboot tolerates it, 0x263b08b).
    EXPECT_EQ(post(h, 1), sce_error(einval));
    EXPECT_EQ(post(h, 0), sce_error(einval));
    EXPECT_EQ(wait(h, 4, nullptr), sce_error(einval));
    EXPECT_EQ(wait(h, 0, nullptr), sce_error(einval));
    EXPECT_EQ(try_wait(h, 2), 0);
    EXPECT_EQ(try_wait(h, 2), sce_error(ebusy));
    EXPECT_EQ(wait(h, 1, nullptr), 0);
    EXPECT_EQ(try_wait(h, 1), sce_error(ebusy));
    EXPECT_EQ(destroy(h), 0);
}

TEST(KernelCountedSync, TimedWaitTimesOutAfterItsMicroseconds) {
    const std::uint64_t h = make(0, 1);
    const std::uint32_t timeout = 30000;
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(wait(h, 1, &timeout), sce_error(etimedout));
    EXPECT_GE(std::chrono::steady_clock::now() - start, 30ms);
    const std::uint32_t zero = 0;
    EXPECT_EQ(wait(h, 1, &zero), sce_error(etimedout));
    EXPECT_EQ(post(h, 1), 0);
    EXPECT_EQ(wait(h, 1, &zero), 0);
    EXPECT_EQ(destroy(h), 0);
}

// Each waiter appends its index to `order` when its request is granted.
struct Granted {
    std::mutex mutex;
    std::vector<int> order;
    void add(int i) {
        const std::lock_guard lock(mutex);
        order.push_back(i);
    }
    std::size_t size() {
        const std::lock_guard lock(mutex);
        return order.size();
    }
};

TEST(KernelCountedSync, PostWakesWaitersInTheOrderOfTheirRequests) {
    const std::uint64_t h = make(0, 10);
    Granted granted;
    const std::int32_t needs[] = {2, 1, 1};
    std::vector<std::thread> threads;
    for (int i = 0; i < 3; ++i) {
        threads.emplace_back([&, i] {
            EXPECT_EQ(wait(h, needs[i], nullptr), 0);
            granted.add(i);
        });
        std::this_thread::sleep_for(20ms);  // queue the requests in order
    }
    // One unit doesn't cover the head's request, so the later ones wait behind it.
    EXPECT_EQ(post(h, 1), 0);
    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(granted.size(), 0u);
    EXPECT_EQ(try_wait(h, 1), sce_error(ebusy));
    EXPECT_EQ(post(h, 2), 0);
    while (granted.size() < 2) {
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_EQ(post(h, 1), 0);
    for (auto& t : threads) {
        t.join();
    }
    // Waiters 0 and 1 were granted by the same post, so they may append in either order.
    ASSERT_EQ(granted.order.size(), 3u);
    EXPECT_EQ(granted.order[2], 2);
    EXPECT_EQ(destroy(h), 0);
}

TEST(KernelCountedSync, TimedOutHeadLetsTheNextRequestThrough) {
    const std::uint64_t h = make(1, 5);
    std::atomic<int> result{-1};
    const std::uint32_t timeout = 200000;
    std::atomic<bool> head_done{false};
    std::thread head([&] {
        EXPECT_EQ(wait(h, 3, &timeout), sce_error(etimedout));
        head_done = true;
    });
    std::this_thread::sleep_for(20ms);
    // The queued head holds back a request that the count would cover.
    EXPECT_EQ(try_wait(h, 1), sce_error(ebusy));
    std::thread next([&] { result = wait(h, 1, nullptr); });
    std::this_thread::sleep_for(20ms);
    EXPECT_FALSE(head_done);
    EXPECT_EQ(result, -1);
    head.join();
    next.join();
    EXPECT_EQ(result, 0);
    EXPECT_EQ(destroy(h), 0);
}

TEST(KernelCountedSync, DestroyWakesWaitersWithEacces) {
    const std::uint64_t h = make(0, 1);
    std::thread waiter([&] { EXPECT_EQ(wait(h, 1, nullptr), sce_error(eacces)); });
    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(destroy(h), 0);
    waiter.join();
}

}  // namespace
