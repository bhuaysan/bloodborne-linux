// Threads and thread attributes, called through their registered NIDs. This test links
// bbeboottls, so every new thread runs the eboot TLS check.

#include "hle/kernel/thread.hpp"

#include <pthread.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>

#include <gtest/gtest.h>

#include "common/log.hpp"
#include "hle/hle.hpp"
#include "loader/dynamic.hpp"

namespace {

using namespace bb::hle::kernel;
using Cell = std::uintptr_t;
using Handle = Thread*;

template <class F>
F* fn(const char* nid, const char* library = "libkernel") {
    return reinterpret_cast<F*>(
        bb::hle::resolve({nid, library, "libTestModule", bb::loader::stt_func}));
}

auto* const create =
    fn<std::int32_t(Handle*, const Cell*, ThreadEntry, void*, const char*)>("6UgtwV+0zb4");
auto* const join = fn<std::int32_t(Handle, void**)>("onNY9Byn-W8");
auto* const exit = fn<void(void*)>("3kg7rT0NQIs");
auto* const attr_init = fn<std::int32_t(Cell*)>("nsYoNRywwNg");
auto* const attr_destroy = fn<std::int32_t(Cell*)>("62KCwEMmzcM");
auto* const attr_setstacksize = fn<std::int32_t(Cell*, std::size_t)>("UTXzJbWhhTE");
auto* const detach = fn<std::int32_t(Handle)>("4qGrR6eoP9Y");
auto* const self = fn<Handle()>("aI+OeCz8xrQ");
auto* const equal = fn<int(Handle, Handle)>("3PtV6p3QNX4");
auto* const yield = fn<void()>("T72hz6ffq08");
auto* const sched_yield = fn<int()>("6XG4B33N09g");
auto* const attr_setdetachstate = fn<std::int32_t(Cell*, int)>("-Wreprtu0Qs");
auto* const attr_setschedparam = fn<std::int32_t(Cell*, const int*)>("DzES9hQF4f4");
auto* const attr_setschedpolicy = fn<std::int32_t(Cell*, int)>("4+h9EzwKF4I");
auto* const attr_setinheritsched = fn<std::int32_t(Cell*, int)>("eXbUSpEaTsA");
auto* const attr_get = fn<std::int32_t(Handle, Cell*)>("x1X76arYMxU");

constexpr const char* posix = "libScePosix";
auto* const posix_create = fn<int(Handle*, const Cell*, ThreadEntry, void*)>("OxhIB8LB-PQ", posix);
auto* const posix_create_named =
    fn<int(Handle*, const Cell*, ThreadEntry, void*, const char*)>("Jmi+9w9u0E4", posix);
auto* const posix_join = fn<int(Handle, void**)>("h9CcP3J0oVM", posix);
auto* const posix_exit = fn<void(void*)>("FJrT5LuUBAU", posix);
auto* const posix_self = fn<Handle()>("EotR8a3ASf4", posix);
auto* const posix_attr_init = fn<int(Cell*)>("wtkt-teR1so", posix);
auto* const posix_attr_destroy = fn<int(Cell*)>("zHchY8ft5pk", posix);
auto* const posix_attr_setstacksize = fn<int(Cell*, std::size_t)>("2Q0z6rnBrTE", posix);
auto* const posix_attr_setdetachstate = fn<int(Cell*, int)>("E+tyo3lp5Lw", posix);
auto* const posix_attr_setschedparam = fn<int(Cell*, const int*)>("euKRgm0Vn2M", posix);

constexpr std::int32_t esrch = 0x80020003, edeadlk = 0x8002000b, einval = 0x80020016,
                       eagain = 0x80020023;

char marker;

void* add_one(void* arg) {
    return static_cast<char*>(arg) + 1;
}

TEST(Thread, CreateAndJoinReturnTheEntrysResult) {
    Cell attr = 0;
    ASSERT_EQ(attr_init(&attr), 0);
    Handle thread = nullptr;
    char buffer[2];
    ASSERT_EQ(create(&thread, &attr, add_one, buffer, "adder"), 0);
    EXPECT_EQ(attr_destroy(&attr), 0);  // the thread keeps its own copy
    void* value = nullptr;
    ASSERT_EQ(join(thread, &value), 0);
    EXPECT_EQ(value, buffer + 1);

    ASSERT_EQ(create(&thread, nullptr, add_one, buffer, nullptr), 0);  // null: the defaults
    EXPECT_EQ(join(thread, nullptr), 0);  // the result may be dropped
}

TEST(Thread, TheNameGoesIntoTheLog) {
    Handle thread = nullptr;
    ASSERT_EQ(create(
                  &thread, nullptr,
                  [](void*) -> void* {
                      return bb::common::thread_name() == "game worker" ? &marker : nullptr;
                  },
                  nullptr, "game worker"),
              0);
    void* value = nullptr;
    ASSERT_EQ(join(thread, &value), 0);
    EXPECT_EQ(value, &marker);
}

// ---- exit --------------------------------------------------------------------------------------

std::atomic<bool> after_exit{false};

[[gnu::noinline]] void nested_exit(void* value) {
    exit(value);
    after_exit = true;
}

void* exit_from_nested_call(void* arg) {
    nested_exit(static_cast<char*>(arg) + 2);
    after_exit = true;
    return nullptr;
}

TEST(Thread, ExitFromANestedCallEndsTheThreadWithItsValue) {
    char buffer[3];
    Handle thread = nullptr;
    ASSERT_EQ(create(&thread, nullptr, exit_from_nested_call, buffer, nullptr), 0);
    void* value = nullptr;
    ASSERT_EQ(join(thread, &value), 0);
    EXPECT_EQ(value, buffer + 2);
    EXPECT_FALSE(after_exit);
}

// ---- stack size --------------------------------------------------------------------------------

void* host_stack_size(void*) {
    pthread_attr_t attr;
    pthread_getattr_np(pthread_self(), &attr);
    std::size_t size = 0;
    pthread_attr_getstacksize(&attr, &size);
    pthread_attr_destroy(&attr);
    return reinterpret_cast<void*>(size);
}

std::size_t stack_size_of(const Cell* attr) {
    Handle thread = nullptr;
    EXPECT_EQ(create(&thread, attr, host_stack_size, nullptr, nullptr), 0);
    void* value = nullptr;
    EXPECT_EQ(join(thread, &value), 0);
    return reinterpret_cast<std::size_t>(value);
}

// The most glibc hands out for a request: it reuses the cached stack of an earlier thread when
// that one is at most four times the size, plus room for the guard page and rounding. For the
// small sizes this stays far below the host's default thread stack (RLIMIT_STACK, 8 MiB or more).
constexpr std::size_t largest_host_stack(std::size_t requested) {
    return 4 * (requested + thread_stack_reserve) + 0x10000;
}

TEST(Thread, TheRequestedStackSizeIsApplied) {
    // The host stack holds at least the requested size plus the runtime's reserve.
    std::size_t size = stack_size_of(nullptr);
    EXPECT_GE(size, default_thread_stack_size + thread_stack_reserve);
    EXPECT_LE(size, largest_host_stack(default_thread_stack_size));

    Cell attr = 0;
    ASSERT_EQ(attr_init(&attr), 0);
    // Above any host default (glibc takes at most 32 MiB when RLIMIT_STACK is unlimited).
    constexpr std::size_t large = 64 << 20;
    ASSERT_EQ(attr_setstacksize(&attr, large), 0);
    EXPECT_GE(stack_size_of(&attr), large + thread_stack_reserve);

    EXPECT_EQ(attr_setstacksize(&attr, min_thread_stack_size - 1), einval);
    ASSERT_EQ(attr_setstacksize(&attr, min_thread_stack_size), 0);
    size = stack_size_of(&attr);
    EXPECT_GE(size, min_thread_stack_size + thread_stack_reserve);
    EXPECT_LE(size, largest_host_stack(min_thread_stack_size));
    EXPECT_EQ(attr_destroy(&attr), 0);
    EXPECT_EQ(attr_destroy(&attr), einval);
}

TEST(Thread, AStackSizeTheHostCantGiveFailsTheCreate) {
    Cell attr = 0;
    ASSERT_EQ(attr_init(&attr), 0);
    Handle thread = reinterpret_cast<Handle>(&marker);  // a failed create leaves it alone
    // Sizes that wrap around with the reserve or the host guard page, and one too large to map.
    for (const std::size_t size : {SIZE_MAX, SIZE_MAX - thread_stack_reserve + 1,
                                   SIZE_MAX - thread_stack_reserve, std::size_t{1} << 62}) {
        SCOPED_TRACE(size);
        ASSERT_EQ(attr_setstacksize(&attr, size), 0);  // the setter only checks the minimum
        EXPECT_EQ(create(&thread, &attr, add_one, nullptr, nullptr), eagain);
        EXPECT_EQ(thread, reinterpret_cast<Handle>(&marker));
    }
    EXPECT_EQ(attr_destroy(&attr), 0);
}

// ---- handles -----------------------------------------------------------------------------------

TEST(Thread, TheHandleIsStoredBeforeTheThreadStarts) {
    // The entry reads the handle variable at once, as game code may.
    for (int i = 0; i < 20; ++i) {
        Handle thread = nullptr;
        ASSERT_EQ(create(
                      &thread, nullptr,
                      [](void* handle) -> void* { return *static_cast<Handle*>(handle); },
                      &thread, nullptr),
                  0);
        void* value = nullptr;
        ASSERT_EQ(join(thread, &value), 0);
        EXPECT_EQ(value, thread);
    }
}

Handle joins_itself = nullptr;

void* join_self(void*) {
    // No wait for the creator: the handle and the thread table entry exist before the start.
    return reinterpret_cast<void*>(static_cast<std::intptr_t>(join(joins_itself, nullptr)));
}

TEST(Thread, JoiningTheCallingThreadIsADeadlock) {
    ASSERT_EQ(create(&joins_itself, nullptr, join_self, nullptr, nullptr), 0);
    void* value = nullptr;
    ASSERT_EQ(join(joins_itself, &value), 0);
    EXPECT_EQ(reinterpret_cast<std::intptr_t>(value), edeadlk);
}

TEST(Thread, InvalidHandlesAndAttributesAreRefused) {
    Cell not_a_thread[8] = {};
    const auto bogus = reinterpret_cast<Handle>(not_a_thread);
    EXPECT_EQ(join(nullptr, nullptr), esrch);
    EXPECT_EQ(join(bogus, nullptr), esrch);

    Handle thread = nullptr;
    ASSERT_EQ(create(&thread, nullptr, add_one, &marker, nullptr), 0);
    ASSERT_EQ(join(thread, nullptr), 0);
    EXPECT_EQ(join(thread, nullptr), esrch);  // already joined

    Cell attr = 0;  // never initialised
    EXPECT_EQ(create(&thread, &attr, add_one, nullptr, nullptr), einval);
    attr = reinterpret_cast<Cell>(not_a_thread);
    EXPECT_EQ(create(&thread, &attr, add_one, nullptr, nullptr), einval);
    EXPECT_EQ(attr_setstacksize(&attr, 0x10000), einval);
    EXPECT_EQ(create(&thread, nullptr, nullptr, nullptr, nullptr), einval);
    EXPECT_EQ(create(nullptr, nullptr, add_one, nullptr, nullptr), einval);
    EXPECT_EQ(attr_init(nullptr), einval);
}

// ---- detach ------------------------------------------------------------------------------------

std::atomic<bool> release{false};

void* wait_for_release(void*) {
    while (!release) {
        std::this_thread::yield();
    }
    return nullptr;
}

// Waits until `thread` has left the thread table, which AttrGet tells by ESRCH.
bool freed(Handle thread) {
    Cell attr = 0;
    EXPECT_EQ(attr_init(&attr), 0);
    std::int32_t result = 0;
    for (int i = 0; i < 2000 && (result = attr_get(thread, &attr)) == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(attr_destroy(&attr), 0);
    return result == esrch;
}

TEST(Thread, DetachedThreadsFreeTheirObjectWhenTheyEnd) {
    Cell attr = 0;
    ASSERT_EQ(attr_init(&attr), 0);
    EXPECT_EQ(attr_setdetachstate(&attr, 2), einval);
    ASSERT_EQ(attr_setdetachstate(&attr, guest_create_detached), 0);
    release = false;
    Handle thread = nullptr;
    ASSERT_EQ(create(&thread, &attr, wait_for_release, nullptr, nullptr), 0);
    EXPECT_EQ(attr_destroy(&attr), 0);
    EXPECT_EQ(join(thread, nullptr), einval);
    EXPECT_EQ(detach(thread), einval);

    Handle running = nullptr;  // detached while it runs
    ASSERT_EQ(create(&running, nullptr, wait_for_release, nullptr, nullptr), 0);
    ASSERT_EQ(detach(running), 0);
    EXPECT_EQ(detach(running), einval);
    EXPECT_EQ(join(running, nullptr), einval);
    release = true;
    EXPECT_TRUE(freed(thread));
    EXPECT_TRUE(freed(running));

    Handle finished = nullptr;  // detached after it ended
    ASSERT_EQ(create(&finished, nullptr, add_one, &marker, nullptr), 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));  // most likely ended by now
    ASSERT_EQ(detach(finished), 0);
    EXPECT_TRUE(freed(finished));
    EXPECT_EQ(detach(nullptr), esrch);
}

// ---- self --------------------------------------------------------------------------------------

TEST(Thread, SelfIsTheHandleTheCreatorGot) {
    Handle thread = nullptr;
    ASSERT_EQ(create(&thread, nullptr, [](void*) -> void* { return self(); }, nullptr, nullptr), 0);
    void* value = nullptr;
    ASSERT_EQ(join(thread, &value), 0);
    EXPECT_EQ(value, thread);
    EXPECT_EQ(equal(self(), self()), 1);
    EXPECT_EQ(equal(self(), thread), 0);
    yield();
    EXPECT_EQ(sched_yield(), 0);
}

TEST(Thread, AThreadTheRuntimeDidntStartGetsAnObjectFreedAtItsExit) {
    Handle other = nullptr;
    std::atomic<bool> started{false};
    release = false;
    std::thread host([&] {
        other = self();
        started = true;
        wait_for_release(nullptr);
    });
    while (!started) {
        std::this_thread::yield();
    }
    EXPECT_EQ(join(other, nullptr), einval);
    EXPECT_EQ(detach(other), einval);
    Cell attr = 0;
    ASSERT_EQ(attr_init(&attr), 0);
    EXPECT_EQ(attr_get(other, &attr), 0);
    release = true;
    host.join();  // returns after the thread's thread_local destructors
    EXPECT_EQ(attr_get(other, &attr), esrch);
    EXPECT_EQ(attr_destroy(&attr), 0);
}

// ---- exit handlers -----------------------------------------------------------------------------

thread_local bool exit_handler_ran = false;
std::atomic<int> probe_result{0};  // 1: the handler ran before the destructor, 2: it didn't

void exit_handler() {
    exit_handler_ran = true;
}

// Constructed on a thread's first use, so only the threads of use_probe have one.
struct Probe {
    bool used = false;
    ~Probe() { probe_result = exit_handler_ran ? 1 : 2; }
};
thread_local Probe probe;

void* use_probe(void* exits) {
    probe.used = true;
    if (exits != nullptr) {
        exit(nullptr);
    }
    return nullptr;
}

TEST(Thread, ExitHandlersRunBeforeTheThreadLocalDestructors) {
    register_thread_exit_handler(exit_handler);
    for (void* exits : {static_cast<void*>(nullptr), static_cast<void*>(&marker)}) {
        SCOPED_TRACE(exits != nullptr ? "exit" : "return");
        probe_result = 0;
        Handle thread = nullptr;
        ASSERT_EQ(create(&thread, nullptr, use_probe, exits, nullptr), 0);
        ASSERT_EQ(join(thread, nullptr), 0);
        EXPECT_EQ(probe_result, 1);
    }
}

// ---- AttrGet and the recorded attributes -------------------------------------------------------

TEST(Thread, AttrGetCopiesTheAttributesAndTheCurrentDetachState) {
    Cell attr = 0;
    ASSERT_EQ(attr_init(&attr), 0);
    constexpr std::size_t large = 64 << 20;
    ASSERT_EQ(attr_setstacksize(&attr, large), 0);
    const int priority = 700;  // recorded, not applied
    EXPECT_EQ(attr_setschedparam(&attr, &priority), 0);
    EXPECT_EQ(attr_setschedparam(&attr, nullptr), einval);
    EXPECT_EQ(attr_setschedpolicy(&attr, 1), 0);
    EXPECT_EQ(attr_setinheritsched(&attr, 0), 0);
    release = false;
    Handle thread = nullptr;
    ASSERT_EQ(create(&thread, &attr, wait_for_release, nullptr, nullptr), 0);
    EXPECT_EQ(attr_destroy(&attr), 0);
    ASSERT_EQ(detach(thread), 0);

    Cell copy = 0;
    ASSERT_EQ(attr_init(&copy), 0);
    ASSERT_EQ(attr_get(thread, &copy), 0);
    Handle second = nullptr;  // detached, as the thread was when it was copied
    ASSERT_EQ(create(&second, &copy, wait_for_release, nullptr, nullptr), 0);
    EXPECT_EQ(join(second, nullptr), einval);
    ASSERT_EQ(attr_setdetachstate(&copy, guest_create_joinable), 0);
    EXPECT_GE(stack_size_of(&copy), large + thread_stack_reserve);
    release = true;
    EXPECT_TRUE(freed(thread));
    EXPECT_TRUE(freed(second));
    EXPECT_EQ(attr_get(thread, &copy), esrch);

    Cell not_an_attr = 0;
    EXPECT_EQ(attr_get(self(), &not_an_attr), einval);
    EXPECT_EQ(attr_setschedpolicy(&not_an_attr, 1), einval);
    EXPECT_EQ(attr_destroy(&copy), 0);
}

// ---- libScePosix -------------------------------------------------------------------------------

void* exit_with_self(void*) {
    posix_exit(posix_self());
    return nullptr;
}

TEST(Thread, ThePosixCallsReturnErrnoNumbers) {
    constexpr int posix_esrch = 3, posix_einval = 22;
    Cell attr = 0;
    ASSERT_EQ(posix_attr_init(&attr), 0);
    EXPECT_EQ(posix_attr_setstacksize(&attr, min_thread_stack_size - 1), posix_einval);
    EXPECT_EQ(posix_attr_setstacksize(&attr, 0x20000), 0);
    EXPECT_EQ(posix_attr_setdetachstate(&attr, 2), posix_einval);
    EXPECT_EQ(posix_attr_setdetachstate(&attr, guest_create_joinable), 0);
    const int priority = 700;
    EXPECT_EQ(posix_attr_setschedparam(&attr, &priority), 0);

    Handle thread = nullptr;
    ASSERT_EQ(posix_create(&thread, &attr, exit_with_self, nullptr), 0);
    void* value = nullptr;
    ASSERT_EQ(posix_join(thread, &value), 0);
    EXPECT_EQ(value, thread);
    EXPECT_EQ(posix_join(thread, nullptr), posix_esrch);

    ASSERT_EQ(posix_create_named(&thread, nullptr, add_one, &marker, "posix worker"), 0);
    EXPECT_EQ(thread->name, "posix worker");
    ASSERT_EQ(posix_join(thread, nullptr), 0);
    EXPECT_EQ(posix_create(&thread, nullptr, nullptr, nullptr), posix_einval);
    EXPECT_EQ(posix_attr_destroy(&attr), 0);
    EXPECT_EQ(posix_attr_destroy(&attr), posix_einval);
}

}  // namespace
