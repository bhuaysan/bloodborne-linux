// The process's fault handler (src/shadps4_host/fault_handler.hpp): a fault that a Core::Signals
// handler claims is retried, any other one still ends the process as before, with
// AddressSanitizer's report in sanitizer builds. Needs no Vulkan device.

#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include <gtest/gtest.h>

#include "common/signal_stack.hpp"
#include "core/signals.h"
#include "shadps4_host/fault_handler.hpp"

#if defined(__SANITIZE_ADDRESS__)
#define BB_TEST_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define BB_TEST_ASAN 1
#endif
#endif

namespace {

// A page that the test handler makes writable when it faults, as the page manager does with a
// page it protected, and how often that happened.
void* tracked_page = nullptr;
int tracked_faults = 0;

bool claim_tracked(void*, void* fault_address) {
    const auto start = reinterpret_cast<std::uintptr_t>(tracked_page);
    const auto at = reinterpret_cast<std::uintptr_t>(fault_address);
    if (tracked_page == nullptr || at < start || at - start >= std::uintptr_t(::getpagesize())) {
        return false;
    }
    ++tracked_faults;
    ::mprotect(tracked_page, ::getpagesize(), PROT_READ | PROT_WRITE);
    return true;
}

// Installs the handler and registers the test handler once per process.
void install() {
    static const bool done = [] {
        tracked_page =
            ::mmap(nullptr, ::getpagesize(), PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        Core::Signals::Instance()->RegisterAccessViolationHandler(claim_tracked, 1);
        bb::shadps4::install_fault_handler();
        return true;
    }();
    (void)done;
}

// How an unclaimed fault ends the process: AddressSanitizer reports it and exits with 1, the
// default action kills it with the signal.
#ifdef BB_TEST_ASAN
#define EXPECT_FAULT_DEATH(statement, asan_report) \
    EXPECT_EXIT(statement, ::testing::ExitedWithCode(1), asan_report)
#else
#define EXPECT_FAULT_DEATH(statement, asan_report) \
    EXPECT_EXIT(statement, ::testing::KilledBySignal(SIGSEGV), "")
#endif

TEST(FaultHandler, AClaimedFaultIsRetried) {
    install();
    ASSERT_NE(tracked_page, MAP_FAILED);
    ::mprotect(tracked_page, ::getpagesize(), PROT_READ);
    const std::uint64_t claimed = bb::shadps4::claimed_faults();
    const int faults = tracked_faults;
    static_cast<volatile std::uint32_t*>(tracked_page)[3] = 0x1234;
    EXPECT_EQ(static_cast<volatile std::uint32_t*>(tracked_page)[3], 0x1234U);
    EXPECT_EQ(tracked_faults, faults + 1);
    EXPECT_EQ(bb::shadps4::claimed_faults(), claimed + 1);
    // The page is writable now: no more faults.
    static_cast<volatile std::uint32_t*>(tracked_page)[4] = 1;
    EXPECT_EQ(bb::shadps4::claimed_faults(), claimed + 1);
}

TEST(FaultHandlerDeathTest, AnUnclaimedFaultEndsTheProcess) {
    install();
    void* const other =
        ::mmap(nullptr, ::getpagesize(), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(other, MAP_FAILED);
    EXPECT_FAULT_DEATH(static_cast<volatile std::uint32_t*>(other)[0] = 1,
                       "AddressSanitizer: SEGV");
    ::munmap(other, ::getpagesize());
}

// A sent SIGSEGV has no fault behind it: it is not dispatched, even with an address the test
// handler would claim, and ends the process.
TEST(FaultHandlerDeathTest, ASentSignalEndsTheProcess) {
    install();
    EXPECT_FAULT_DEATH(
        {
            ::mprotect(tracked_page, ::getpagesize(), PROT_READ);
            siginfo_t info{};
            info.si_signo = SIGSEGV;
            info.si_code = SI_QUEUE;
            info.si_addr = tracked_page;
            ::syscall(SYS_rt_tgsigqueueinfo, ::getpid(), ::gettid(), SIGSEGV, &info);
            ::_exit(0);  // only if the signal was claimed
        },
        "AddressSanitizer: SEGV");
}

[[gnu::noinline]] int recurse(int depth) {
    volatile char frame[1024];
    frame[0] = static_cast<char>(depth);
    if (depth == INT32_MAX) {  // never: the stack ends long before
        return 0;
    }
    return recurse(depth + 1) + frame[0];
}

void* overflow(void*) {
    bb::common::ensure_signal_stack();
    std::exit(recurse(0));
}

// A game thread that runs out of stack ends the process: the handler runs on the alternate
// signal stack and passes the fault on.
TEST(FaultHandlerDeathTest, AStackOverflowEndsTheProcess) {
    install();
    EXPECT_FAULT_DEATH(
        {
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setstacksize(&attr, std::size_t{256} << 10);
            pthread_t thread;
            pthread_create(&thread, &attr, overflow, nullptr);
            pthread_join(thread, nullptr);
        },
        "AddressSanitizer: stack-overflow");
}

struct AltStack {
    bool enabled;
    void* sp;
    std::size_t size;
};

AltStack alt_stack() {
    stack_t s{};
    ::sigaltstack(nullptr, &s);
    return {(s.ss_flags & SS_DISABLE) == 0, s.ss_sp, s.ss_size};
}

// Each thread records its alternate stack after one call and after a second one.
struct Seen {
    AltStack first{}, second{};
};

void* without_stack(void* p) {
    stack_t off{};
    off.ss_flags = SS_DISABLE;
    ::sigaltstack(&off, nullptr);  // as a thread starts without AddressSanitizer
    auto* seen = static_cast<Seen*>(p);
    bb::common::ensure_signal_stack();
    seen->first = alt_stack();
    bb::common::ensure_signal_stack();
    seen->second = alt_stack();
    return nullptr;
}

alignas(16) char own_stack[64 << 10];

void* with_stack(void* p) {
    stack_t mine{};
    mine.ss_sp = own_stack;
    mine.ss_size = sizeof own_stack;
    ::sigaltstack(&mine, nullptr);
    auto* seen = static_cast<Seen*>(p);
    bb::common::ensure_signal_stack();
    seen->first = alt_stack();
    // Off again before the thread ends: AddressSanitizer unmaps the stack it finds then.
    stack_t off{};
    off.ss_flags = SS_DISABLE;
    ::sigaltstack(&off, nullptr);
    return nullptr;
}

// One is installed where there is none, and kept by a second call; an existing one is kept.
TEST(SignalStack, IsInstalledOnlyWhereThereIsNone) {
    Seen fresh, kept;
    pthread_t thread;
    ASSERT_EQ(pthread_create(&thread, nullptr, without_stack, &fresh), 0);
    pthread_join(thread, nullptr);
    EXPECT_TRUE(fresh.first.enabled);
    EXPECT_EQ(fresh.first.size, std::size_t{128} << 10);
    EXPECT_EQ(fresh.second.sp, fresh.first.sp);
    ASSERT_EQ(pthread_create(&thread, nullptr, with_stack, &kept), 0);
    pthread_join(thread, nullptr);
    EXPECT_TRUE(kept.first.enabled);
    EXPECT_EQ(kept.first.sp, own_stack);
    EXPECT_EQ(kept.first.size, sizeof own_stack);
}

}  // namespace
