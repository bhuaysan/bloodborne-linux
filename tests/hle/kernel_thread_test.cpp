// Thread creation, join and exit, called through their registered NIDs. This test links
// bbeboottls, so new threads run the eboot TLS check and game-style code can use fs.

#include "hle/kernel/thread.hpp"

#include <pthread.h>
#include <sys/mman.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <latch>
#include <thread>

#include <gtest/gtest.h>

#include "common/log.hpp"
#include "hle/hle.hpp"
#include "hle/kernel/tls.hpp"
#include "loader/dynamic.hpp"

namespace {

using namespace bb::hle::kernel;
using Cell = std::uintptr_t;
using Handle = Thread*;

template <class F>
F* fn(const char* nid) {
    return reinterpret_cast<F*>(
        bb::hle::resolve({nid, "libkernel", "libTestModule", bb::loader::stt_func}));
}

auto* const create =
    fn<std::int32_t(Handle*, const Cell*, ThreadEntry, void*, const char*)>("6UgtwV+0zb4");
auto* const join = fn<std::int32_t(Handle, void**)>("onNY9Byn-W8");
auto* const exit = fn<void(void*)>("3kg7rT0NQIs");
auto* const attr_init = fn<std::int32_t(Cell*)>("nsYoNRywwNg");
auto* const attr_destroy = fn<std::int32_t(Cell*)>("62KCwEMmzcM");
auto* const attr_setstacksize = fn<std::int32_t(Cell*, std::size_t)>("UTXzJbWhhTE");

constexpr std::int32_t esrch = 0x80020003, edeadlk = 0x8002000b, einval = 0x80020016;

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

TEST(Thread, TheRequestedStackSizeIsApplied) {
    // The host stack holds at least the requested size plus the runtime's reserve. It can be
    // larger: glibc rounds it up and reuses the cached stack of an earlier thread when that one
    // is up to four times the size.
    EXPECT_GE(stack_size_of(nullptr), default_thread_stack_size + thread_stack_reserve);
    Cell attr = 0;
    ASSERT_EQ(attr_init(&attr), 0);
    ASSERT_EQ(attr_setstacksize(&attr, 0x100000), 0);
    EXPECT_GE(stack_size_of(&attr), 0x100000 + thread_stack_reserve);
    EXPECT_EQ(attr_setstacksize(&attr, min_thread_stack_size - 1), einval);
    ASSERT_EQ(attr_setstacksize(&attr, min_thread_stack_size), 0);
    EXPECT_GE(stack_size_of(&attr), min_thread_stack_size + thread_stack_reserve);
    EXPECT_EQ(attr_destroy(&attr), 0);
    EXPECT_EQ(attr_destroy(&attr), einval);
}

// ---- handles -----------------------------------------------------------------------------------

std::latch* handle_stored = nullptr;
Handle joins_itself = nullptr;

void* join_self(void*) {
    handle_stored->wait();
    return reinterpret_cast<void*>(static_cast<std::intptr_t>(join(joins_itself, nullptr)));
}

TEST(Thread, JoiningTheCallingThreadIsADeadlock) {
    std::latch stored(1);
    handle_stored = &stored;
    ASSERT_EQ(create(&joins_itself, nullptr, join_self, nullptr, nullptr), 0);
    stored.count_down();
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

// ---- game code ---------------------------------------------------------------------------------

// Game-style code that uses the eboot's TLS block as the eboot does (fs:0 plus a negative offset):
//   mov rax, fs:0; mov rcx, [rax-0x750]; mov [rax-0x750], rdi; add rcx, [rax-0x750];
//   mov rax, rcx; ret
// It returns its argument plus what the slot held before, which is 0 on a new thread.
constexpr std::array<unsigned char, 34> tls_entry = {
    0x64, 0x48, 0x8b, 0x04, 0x25, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x88,
    0xb0, 0xf8, 0xff, 0xff, 0x48, 0x89, 0xb8, 0xb0, 0xf8, 0xff, 0xff, 0x48,
    0x03, 0x88, 0xb0, 0xf8, 0xff, 0xff, 0x48, 0x89, 0xc8, 0xc3};

TEST(Thread, AGameStyleEntryReadsItsTlsOnTheNewThread) {
    void* page = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(page, MAP_FAILED);
    std::memcpy(page, tls_entry.data(), tls_entry.size());
    ASSERT_EQ(mprotect(page, 4096, PROT_READ | PROT_EXEC), 0);
    auto* slot = reinterpret_cast<std::uint64_t*>(eboot_tls_placement()->block);
    *slot = 0x1000;  // the creating thread's slot, which the new thread must not see

    Handle thread = nullptr;
    ASSERT_EQ(create(&thread, nullptr, reinterpret_cast<ThreadEntry>(page),
                     reinterpret_cast<void*>(0x5a5a), "tls"),
              0);
    void* value = nullptr;
    ASSERT_EQ(join(thread, &value), 0);
    EXPECT_EQ(value, reinterpret_cast<void*>(0x5a5a));
    EXPECT_EQ(*slot, 0x1000U);
    *slot = 0;
    munmap(page, 4096);
}

}  // namespace
