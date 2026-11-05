#include "hle/kernel/equeue.hpp"

#include <elf.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "hle/hle.hpp"
#include "loader/dynamic.hpp"

namespace {

using bb::hle::EventQueue;
using bb::hle::GuestEvent;
using namespace bb::hle::kernel;

constexpr std::int32_t timed_out = static_cast<std::int32_t>(0x8002003c);  // ETIMEDOUT 60
constexpr std::int32_t canceled = static_cast<std::int32_t>(0x80020055);   // ECANCELED 85
constexpr std::int32_t bad_handle = static_cast<std::int32_t>(0x80020009);  // EBADF 9
constexpr std::int32_t invalid = static_cast<std::int32_t>(0x80020016);     // EINVAL 22

std::uint64_t create_queue() {
    std::uint64_t handle = 0;
    EXPECT_EQ(equeue_create(&handle, "TestQueue"), 0);
    EXPECT_NE(handle, 0u);
    return handle;
}

GuestEvent event(std::uint64_t id, std::int16_t kind = 1, std::int64_t data = 0) {
    GuestEvent e;
    e.id = id;
    e.kind = kind;
    e.data = data;
    e.user_data = id * 10;
    return e;
}

TEST(KernelEqueue, CreateAndDelete) {
    const std::uint64_t a = create_queue();
    const std::uint64_t b = create_queue();
    EXPECT_NE(a, b);
    EXPECT_EQ(EventQueue::find(a)->name(), "TestQueue");
    EXPECT_EQ(equeue_delete(a), 0);
    EXPECT_EQ(equeue_delete(a), bad_handle);
    EXPECT_EQ(EventQueue::find(a), nullptr);
    EXPECT_EQ(equeue_delete(b), 0);
    EXPECT_EQ(equeue_delete(0), bad_handle);
    EXPECT_EQ(equeue_create(nullptr, "x"), invalid);

    std::uint64_t unnamed = 0;
    EXPECT_EQ(equeue_create(&unnamed, nullptr), 0);
    EXPECT_EQ(EventQueue::find(unnamed)->name(), "");
    EXPECT_EQ(equeue_delete(unnamed), 0);
}

TEST(KernelEqueue, WaitReturnsTriggeredEventsInOrder) {
    const std::uint64_t q = create_queue();
    EXPECT_TRUE(EventQueue::trigger(q, event(1)));
    EXPECT_TRUE(EventQueue::trigger(q, event(2)));
    EXPECT_TRUE(EventQueue::trigger(q, event(3)));

    GuestEvent out[2]{};
    std::int32_t count = -1;
    EXPECT_EQ(equeue_wait(q, out, 2, &count, nullptr), 0);
    ASSERT_EQ(count, 2);
    EXPECT_EQ(out[0], event(1));
    EXPECT_EQ(out[1], event(2));

    EXPECT_EQ(equeue_wait(q, out, 2, &count, nullptr), 0);
    ASSERT_EQ(count, 1);
    EXPECT_EQ(out[0], event(3));
    EXPECT_EQ(equeue_delete(q), 0);
}

TEST(KernelEqueue, PendingEventWithTheSameSourceIsReplaced) {
    const std::uint64_t q = create_queue();
    EventQueue::trigger(q, event(1, 1, 100));
    EventQueue::trigger(q, event(2, 1));
    EventQueue::trigger(q, event(1, 1, 101));  // replaces the first, keeps its place
    EventQueue::trigger(q, event(1, 2));       // other kind: a new record

    GuestEvent out[4]{};
    std::int32_t count = 0;
    EXPECT_EQ(equeue_wait(q, out, 4, &count, nullptr), 0);
    ASSERT_EQ(count, 3);
    EXPECT_EQ(out[0], event(1, 1, 101));
    EXPECT_EQ(out[1], event(2, 1));
    EXPECT_EQ(out[2], event(1, 2));
    EXPECT_EQ(equeue_delete(q), 0);
}

TEST(KernelEqueue, TimeoutReturnsTheTimeoutResult) {
    const std::uint64_t q = create_queue();
    GuestEvent out[1]{};
    std::int32_t count = -1;

    const std::uint32_t poll = 0;
    EXPECT_EQ(equeue_wait(q, out, 1, &count, &poll), timed_out);
    EXPECT_EQ(count, 0);

    const std::uint32_t timeout_us = 20000;
    const auto start = std::chrono::steady_clock::now();
    count = -1;
    EXPECT_EQ(equeue_wait(q, out, 1, &count, &timeout_us), timed_out);
    EXPECT_GE(std::chrono::steady_clock::now() - start, std::chrono::microseconds(timeout_us));
    EXPECT_EQ(count, 0);

    // A pending event is returned at once, even with a zero timeout.
    EventQueue::trigger(q, event(7));
    EXPECT_EQ(equeue_wait(q, out, 1, &count, &poll), 0);
    EXPECT_EQ(count, 1);
    EXPECT_EQ(out[0], event(7));
    EXPECT_EQ(equeue_delete(q), 0);
}

TEST(KernelEqueue, DeleteWakesAWaiter) {
    const std::uint64_t q = create_queue();
    std::atomic<std::int32_t> result = 0;
    std::int32_t count = -1;
    std::thread waiter([&] {
        GuestEvent out[1]{};
        result = equeue_wait(q, out, 1, &count, nullptr);
    });
    const auto queue = EventQueue::find(q);
    while (queue->waiting() == 0) {
        std::this_thread::yield();
    }
    EXPECT_EQ(equeue_delete(q), 0);
    waiter.join();
    EXPECT_EQ(result.load(), canceled);
    EXPECT_EQ(count, 0);
    EXPECT_FALSE(EventQueue::trigger(q, event(1)));
}

TEST(KernelEqueue, TwoProducersOneWaiter) {
    const std::uint64_t q = create_queue();
    constexpr std::uint64_t per_producer = 500;
    const auto produce = [q](std::int16_t kind) {
        for (std::uint64_t i = 0; i < per_producer; ++i) {
            EventQueue::trigger(q, event(i, kind));
        }
    };
    std::thread a(produce, std::int16_t{1});
    std::thread b(produce, std::int16_t{2});

    std::vector<std::uint64_t> next(3, 0);
    std::uint64_t received = 0;
    while (received < 2 * per_producer) {
        GuestEvent out[8]{};
        std::int32_t count = 0;
        const std::uint32_t timeout_us = 5'000'000;
        ASSERT_EQ(equeue_wait(q, out, 8, &count, &timeout_us), 0);
        ASSERT_GE(count, 1);
        for (std::int32_t i = 0; i < count; ++i) {
            const GuestEvent& e = out[i];
            ASSERT_TRUE(e.kind == 1 || e.kind == 2);
            EXPECT_EQ(e.id, next[e.kind]++);  // each producer's events in order
            ++received;
        }
    }
    a.join();
    b.join();
    EXPECT_EQ(next[1], per_producer);
    EXPECT_EQ(next[2], per_producer);
    EXPECT_EQ(equeue_delete(q), 0);
}

TEST(KernelEqueue, WaitRejectsBadArguments) {
    const std::uint64_t q = create_queue();
    GuestEvent out[1]{};
    std::int32_t count = 0;
    EXPECT_EQ(equeue_wait(q, nullptr, 1, &count, nullptr), invalid);
    EXPECT_EQ(equeue_wait(q, out, 0, &count, nullptr), invalid);
    EXPECT_EQ(equeue_wait(q, out, 1, nullptr, nullptr), invalid);
    EXPECT_EQ(equeue_wait(q + 1000, out, 1, &count, nullptr), bad_handle);
    EXPECT_EQ(equeue_delete(q), 0);
}

TEST(KernelEqueue, ImportsResolveToTheImplementations) {
    const auto resolve = [](const char* nid) {
        return bb::hle::resolve(bb::loader::Import{nid, "libkernel", "libkernel", STT_FUNC});
    };
    EXPECT_EQ(resolve("D0OdFMjp46I"), reinterpret_cast<std::uintptr_t>(&equeue_create));
    EXPECT_EQ(resolve("jpFjmgAC5AE"), reinterpret_cast<std::uintptr_t>(&equeue_delete));
    EXPECT_EQ(resolve("fzyMKs9kim0"), reinterpret_cast<std::uintptr_t>(&equeue_wait));
}

}  // namespace
