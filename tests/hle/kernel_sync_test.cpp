// Mutexes, mutex attributes and rwlocks, called through their registered NIDs.

#include "hle/kernel/sync.hpp"

#include <pthread.h>

#include <chrono>
#include <cstdint>
#include <thread>

#include <gtest/gtest.h>

#include "hle/hle.hpp"
#include "hle/kernel/abi.hpp"
#include "loader/dynamic.hpp"
#include "support/hle.hpp"

namespace {

using namespace bb::hle::kernel;
using Cell = std::uintptr_t;
using Attr = std::uint64_t;

using bb::test::fn;

constexpr const char* kernel = "libkernel";
constexpr const char* posix = "libScePosix";

// sce entry points
auto* const sce_attr_init = fn<std::int32_t(Attr*)>(kernel, "F8bUHwAG284");
auto* const sce_attr_destroy = fn<std::int32_t(Attr*)>(kernel, "smWEktiyyG0");
auto* const sce_attr_settype = fn<std::int32_t(Attr*, int)>(kernel, "iMp8QpE+XO4");
auto* const sce_attr_setprotocol = fn<std::int32_t(Attr*, int)>(kernel, "1FGvU0i9saQ");
auto* const sce_init = fn<std::int32_t(Cell*, const Attr*, const char*)>(kernel, "cmo1RIYva9o");
auto* const sce_destroy = fn<std::int32_t(Cell*)>(kernel, "2Of0f+3mhhE");
auto* const sce_lock = fn<std::int32_t(Cell*)>(kernel, "9UK1vLZQft4");
auto* const sce_trylock = fn<std::int32_t(Cell*)>(kernel, "upoVrzMHFeE");
auto* const sce_timedlock = fn<std::int32_t(Cell*, std::uint32_t)>(kernel, "IafI2PxcPnQ");
auto* const sce_unlock = fn<std::int32_t(Cell*)>(kernel, "tn3VlD0hG60");

// POSIX entry points
auto* const px_attr_init = fn<int(Attr*)>(posix, "dQHWEsJtoE4");
auto* const px_attr_destroy = fn<int(Attr*)>(posix, "HF7lK46xzjY");
auto* const px_attr_settype = fn<int(Attr*, int)>(posix, "mDmgMOGVUqg");
auto* const px_init = fn<int(Cell*, const Attr*)>(posix, "ttHNfU+qDBU");
auto* const px_destroy = fn<int(Cell*)>(posix, "ltCfaGr2JGE");
auto* const px_lock = fn<int(Cell*)>(posix, "7H0iTOciTLo");
auto* const px_trylock = fn<int(Cell*)>(posix, "K-jXhbt2gn4");
auto* const px_unlock = fn<int(Cell*)>(posix, "2Z+PpY6CaJg");

// rwlocks
auto* const rw_init = fn<std::int32_t(Cell*, const void*, const char*)>(kernel, "6ULAa0fq4jA");
auto* const rw_destroy = fn<std::int32_t(Cell*)>(kernel, "BB+kb08Tl9A");
auto* const rw_rdlock = fn<std::int32_t(Cell*)>(kernel, "Ox9i0c7L5w0");
auto* const rw_wrlock = fn<std::int32_t(Cell*)>(kernel, "mqdNorrB+gI");
auto* const rw_tryrdlock = fn<std::int32_t(Cell*)>(kernel, "XD3mDeybCnk");
auto* const rw_trywrlock = fn<std::int32_t(Cell*)>(kernel, "bIHoZCTomsI");
auto* const rw_unlock = fn<std::int32_t(Cell*)>(kernel, "+L98PIbGttk");
auto* const rw_timedrdlock = fn<std::int32_t(Cell*, std::uint32_t)>(kernel, "iPtZRWICjrM");
auto* const rw_timedwrlock = fn<std::int32_t(Cell*, std::uint32_t)>(kernel, "adh--6nIqTk");

// Guest errno values.
constexpr int eperm = 1, edeadlk = 11, ebusy = 16, einval = 22, etimedout = 60;

// Runs `f` on another thread and returns its result.
template <class F>
auto on_thread(F f) {
    decltype(f()) result{};
    std::thread([&] { result = f(); }).join();
    return result;
}

// A mutex of `type` made through the sce entry points.
Cell sce_mutex(int type) {
    Attr attr = 0;
    EXPECT_EQ(sce_attr_init(&attr), 0);
    EXPECT_EQ(sce_attr_settype(&attr, type), 0);
    EXPECT_EQ(sce_attr_setprotocol(&attr, guest_prio_inherit), 0);
    Cell m = 0;
    EXPECT_EQ(sce_init(&m, &attr, "test"), 0);
    EXPECT_EQ(sce_attr_destroy(&attr), 0);
    EXPECT_NE(m, 0u);
    return m;
}

TEST(KernelMutex, ErrorcheckAndAdaptiveReportRelockAndForeignUnlock) {
    for (const int type : {guest_mutex_errorcheck, guest_mutex_adaptive}) {
        Cell m = sce_mutex(type);
        EXPECT_EQ(sce_lock(&m), 0);
        EXPECT_EQ(sce_lock(&m), sce_error(edeadlk));
        EXPECT_EQ(sce_trylock(&m), sce_error(ebusy));
        EXPECT_EQ(on_thread([&] { return sce_unlock(&m); }), sce_error(eperm));
        EXPECT_EQ(sce_unlock(&m), 0);
        EXPECT_EQ(sce_unlock(&m), sce_error(eperm));
        EXPECT_EQ(sce_destroy(&m), 0);
        EXPECT_EQ(m, 0u);
    }
}

TEST(KernelMutex, RecursiveCountsItsLocks) {
    Cell m = sce_mutex(guest_mutex_recursive);
    EXPECT_EQ(sce_lock(&m), 0);
    EXPECT_EQ(sce_lock(&m), 0);
    EXPECT_EQ(sce_trylock(&m), 0);
    EXPECT_EQ(on_thread([&] { return sce_trylock(&m); }), sce_error(ebusy));
    EXPECT_EQ(sce_unlock(&m), 0);
    EXPECT_EQ(sce_unlock(&m), 0);
    EXPECT_EQ(on_thread([&] { return sce_trylock(&m); }), sce_error(ebusy));
    EXPECT_EQ(sce_unlock(&m), 0);
    EXPECT_EQ(sce_unlock(&m), sce_error(eperm));
    EXPECT_EQ(sce_destroy(&m), 0);
}

TEST(KernelMutex, NormalRefusesTrylockByItsOwner) {
    Cell m = sce_mutex(guest_mutex_normal);
    EXPECT_EQ(sce_lock(&m), 0);
    EXPECT_EQ(sce_trylock(&m), sce_error(ebusy));
    EXPECT_EQ(sce_unlock(&m), 0);
    EXPECT_EQ(sce_trylock(&m), 0);
    EXPECT_EQ(sce_unlock(&m), 0);
    EXPECT_EQ(sce_destroy(&m), 0);
}

TEST(KernelMutex, DefaultAttributesAndStaticInitialiserAreErrorcheck) {
    Cell made = 0;
    EXPECT_EQ(px_init(&made, nullptr), 0);
    Cell still_static = 0;
    for (Cell* m : {&made, &still_static}) {
        EXPECT_EQ(px_lock(m), 0);
        EXPECT_EQ(px_lock(m), edeadlk);
        EXPECT_EQ(px_unlock(m), 0);
        EXPECT_EQ(px_destroy(m), 0);
    }
}

TEST(KernelMutex, TrylockAndTimedlockFailWhileAnotherThreadHoldsIt) {
    Cell m = sce_mutex(guest_mutex_errorcheck);
    ASSERT_EQ(sce_lock(&m), 0);
    EXPECT_EQ(on_thread([&] { return sce_trylock(&m); }), sce_error(ebusy));
    EXPECT_EQ(on_thread([&] { return px_trylock(&m); }), ebusy);
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(on_thread([&] { return sce_timedlock(&m, 20000); }), sce_error(etimedout));
    EXPECT_GE(std::chrono::steady_clock::now() - start, std::chrono::microseconds(20000));
    EXPECT_EQ(sce_destroy(&m), sce_error(ebusy));
    EXPECT_EQ(sce_unlock(&m), 0);
    EXPECT_EQ(on_thread([&] {
                  const int locked = sce_timedlock(&m, 20000);
                  const int unlocked = sce_unlock(&m);
                  return locked != 0 ? locked : unlocked;
              }),
              0);
    EXPECT_EQ(sce_destroy(&m), 0);
}

TEST(KernelMutex, StaticMutexIsCreatedOnceForTwoThreads) {
    for (int round = 0; round < 20; ++round) {
        Cell m = 0;
        long counter = 0;
        auto work = [&] {
            for (int i = 0; i < 2000; ++i) {
                EXPECT_EQ(sce_lock(&m), 0);
                ++counter;
                EXPECT_EQ(sce_unlock(&m), 0);
            }
        };
        std::thread a(work), b(work);
        a.join();
        b.join();
        EXPECT_EQ(counter, 4000);
        EXPECT_EQ(sce_destroy(&m), 0);
    }
}

TEST(KernelMutex, SceAndPosixReportTheSameFailureInTheirOwnForm) {
    Attr attr = 0;
    EXPECT_EQ(sce_attr_settype(&attr, guest_mutex_normal), sce_error(einval));  // not initialised
    EXPECT_EQ(px_attr_settype(&attr, guest_mutex_normal), einval);
    ASSERT_EQ(px_attr_init(&attr), 0);
    EXPECT_EQ(sce_attr_settype(&attr, 5), sce_error(einval));
    EXPECT_EQ(sce_attr_setprotocol(&attr, 3), sce_error(einval));
    EXPECT_EQ(px_attr_settype(&attr, 0), einval);
    EXPECT_EQ(px_attr_settype(&attr, guest_mutex_recursive), 0);
    EXPECT_EQ(px_attr_destroy(&attr), 0);
    EXPECT_EQ(px_attr_destroy(&attr), einval);
    Cell unused = 0;
    EXPECT_EQ(sce_init(&unused, &attr, nullptr), sce_error(einval));  // destroyed attribute

    Cell garbage = 1;
    EXPECT_EQ(sce_lock(nullptr), sce_error(einval));
    EXPECT_EQ(px_lock(nullptr), einval);
    EXPECT_EQ(sce_lock(&garbage), sce_error(einval));
    EXPECT_EQ(px_unlock(&garbage), einval);
    EXPECT_EQ(sce_destroy(&garbage), sce_error(einval));
    EXPECT_EQ(px_destroy(&garbage), einval);

    Cell m = 0;
    ASSERT_EQ(px_lock(&m), 0);
    EXPECT_EQ(sce_lock(&m), sce_error(edeadlk));
    EXPECT_EQ(px_lock(&m), edeadlk);
    EXPECT_EQ(sce_destroy(&m), sce_error(ebusy));
    EXPECT_EQ(px_destroy(&m), ebusy);
    EXPECT_EQ(on_thread([&] { return px_unlock(&m); }), eperm);
    EXPECT_EQ(px_unlock(&m), 0);
    EXPECT_EQ(px_destroy(&m), 0);
}

TEST(KernelMutex, HostMutexIsTheGuestMutex) {
    Cell m = 0;
    int error = -1;
    pthread_mutex_t* host = host_mutex(&m, &error);
    ASSERT_NE(host, nullptr);
    EXPECT_EQ(error, 0);
    EXPECT_EQ(host_mutex(&m, &error), host);
    ASSERT_EQ(pthread_mutex_lock(host), 0);
    EXPECT_EQ(on_thread([&] { return sce_trylock(&m); }), sce_error(ebusy));
    EXPECT_EQ(sce_unlock(&m), 0);
    EXPECT_EQ(sce_destroy(&m), 0);

    Cell garbage = 1;
    EXPECT_EQ(host_mutex(&garbage, &error), nullptr);
    EXPECT_EQ(error, EINVAL);
    EXPECT_EQ(host_mutex(nullptr, &error), nullptr);
}

TEST(KernelRwlock, ReadersShareAndWritersExclude) {
    Cell rw = 0;
    ASSERT_EQ(rw_init(&rw, nullptr, "test"), 0);
    ASSERT_EQ(rw_rdlock(&rw), 0);
    EXPECT_EQ(on_thread([&] { return rw_tryrdlock(&rw) + rw_unlock(&rw); }), 0);
    EXPECT_EQ(on_thread([&] { return rw_trywrlock(&rw); }), sce_error(ebusy));
    EXPECT_EQ(on_thread([&] { return rw_timedwrlock(&rw, 20000); }), sce_error(etimedout));
    EXPECT_EQ(rw_destroy(&rw), sce_error(ebusy));
    EXPECT_EQ(rw_unlock(&rw), 0);

    ASSERT_EQ(rw_wrlock(&rw), 0);
    EXPECT_EQ(rw_wrlock(&rw), sce_error(edeadlk));
    EXPECT_EQ(on_thread([&] { return rw_tryrdlock(&rw); }), sce_error(ebusy));
    EXPECT_EQ(on_thread([&] { return rw_timedrdlock(&rw, 20000); }), sce_error(etimedout));
    EXPECT_EQ(rw_destroy(&rw), sce_error(ebusy));
    EXPECT_EQ(rw_unlock(&rw), 0);
    EXPECT_EQ(on_thread([&] { return rw_timedwrlock(&rw, 20000) + rw_unlock(&rw); }), 0);
    EXPECT_EQ(rw_destroy(&rw), 0);
    EXPECT_EQ(rw, 0u);
}

TEST(KernelRwlock, StaticRwlockWorksFromTwoThreadsAndRefusesGarbage) {
    Cell rw = 0;
    long counter = 0;
    auto work = [&] {
        for (int i = 0; i < 2000; ++i) {
            EXPECT_EQ(rw_wrlock(&rw), 0);
            ++counter;
            EXPECT_EQ(rw_unlock(&rw), 0);
        }
    };
    std::thread a(work), b(work);
    a.join();
    b.join();
    EXPECT_EQ(counter, 4000);
    EXPECT_EQ(rw_destroy(&rw), 0);

    Cell garbage = 1;
    EXPECT_EQ(rw_rdlock(&garbage), sce_error(einval));
    EXPECT_EQ(rw_rdlock(nullptr), sce_error(einval));
    EXPECT_EQ(rw_init(nullptr, nullptr, nullptr), sce_error(einval));
}

}  // namespace
