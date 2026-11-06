// The process basics of src/hle/kernel/process.cpp, called through bb::hle::resolve as the game
// calls them.

#include <signal.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "hle/hle.hpp"
#include "hle/kernel/abi.hpp"
#include "hle/kernel/process.hpp"
#include "loader/dynamic.hpp"

namespace {

using bb::loader::Import;

template <class F>
F* function(const char* nid) {
    const Import imp{nid, "libkernel", "libkernel", bb::loader::stt_func};
    auto* f = reinterpret_cast<F*>(bb::hle::resolve(imp));
    EXPECT_EQ(bb::hle::binding("libkernel", nid), bb::hle::Binding::implemented) << nid;
    return f;
}

const std::byte* data(const char* nid) {
    const Import imp{nid, "libkernel", "libkernel", bb::loader::stt_object};
    return reinterpret_cast<const std::byte*>(bb::hle::resolve(imp));
}

using ErrorFn = int*();

TEST(KernelProcess, ErrnoLocationIsPerThreadAndStable) {
    auto* error = function<ErrorFn>("9BcDykPmo1I");
    int* mine = error();
    EXPECT_EQ(mine, error());
    EXPECT_EQ(mine, bb::hle::kernel::guest_errno_location());
    int* other_first = nullptr;
    int* other_second = nullptr;
    std::thread([&] {
        other_first = error();
        other_second = error();
        *other_first = 7;
    }).join();
    EXPECT_NE(other_first, nullptr);
    EXPECT_EQ(other_first, other_second);
    EXPECT_NE(other_first, mine);
    EXPECT_NE(*mine, 7);
}

TEST(KernelProcess, StackGuardIsSetWhenResolved) {
    const std::byte* block = data("f7uOxY9mM1U");
    std::uint64_t guard = 0;
    std::memcpy(&guard, block, sizeof guard);
    EXPECT_NE(guard, 0U);
    EXPECT_EQ(data("f7uOxY9mM1U"), block);
    std::uint64_t again = 0;
    std::memcpy(&again, block, sizeof again);
    EXPECT_EQ(again, guard);
}

TEST(KernelProcessDeathTest, StackCheckFailAborts) {
    auto* fail = function<void()>("Ou3iL1abvng");
    EXPECT_DEATH(fail(), "stack guard overwritten");
}

TEST(KernelProcess, ProcessIdAndName) {
    EXPECT_EQ(function<std::int32_t()>("HoLVWNanBBc")(), ::getpid());
    const std::byte* block = data("djxxOmW6-aw");
    const char* name = nullptr;
    std::memcpy(&name, block, sizeof name);
    EXPECT_STREQ(name, "eboot.bin");
}

TEST(KernelProcess, ProcessParametersAreTheBlockTheRuntimeSet) {
    auto* parameters = function<const void*()>("959qrazPIrg");
    testing::internal::CaptureStderr();
    EXPECT_EQ(parameters(), nullptr);
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_NE(log.find("before they were set"), std::string::npos) << log;

    const std::uint64_t block[8] = {0x40};  // the eboot's starts with its size
    bb::hle::kernel::set_process_parameters(reinterpret_cast<std::uintptr_t>(block));
    EXPECT_EQ(parameters(), block);
    std::thread([&] { EXPECT_EQ(parameters(), block); }).join();
    bb::hle::kernel::set_process_parameters(0);
}

using SysctlFn = std::int32_t(const std::int32_t*, std::uint32_t, void*, std::uint64_t*,
                              const void*, std::uint64_t);

TEST(KernelProcess, SysctlReportsAnEmptyVmMapAndRefusesTheRest) {
    auto* sysctl = function<SysctlFn>("DFmMT80xcNI");
    const std::int32_t vmmap[] = {1, 14, 32, ::getpid()};
    std::uint64_t length = 123;
    EXPECT_EQ(sysctl(vmmap, 4, nullptr, &length, nullptr, 0), 0);
    EXPECT_EQ(length, 0U);

    *bb::hle::kernel::guest_errno_location() = 0;
    const std::int32_t other[] = {1, 6};  // not a MIB libc.prx passes
    length = 8;
    char buffer[8] = {};
    EXPECT_EQ(sysctl(other, 2, buffer, &length, nullptr, 0), -1);
    EXPECT_EQ(*bb::hle::kernel::guest_errno_location(), 2);  // ENOENT
    EXPECT_EQ(sysctl(vmmap, 4, nullptr, nullptr, nullptr, 0), -1);
    EXPECT_EQ(*bb::hle::kernel::guest_errno_location(), 14);  // EFAULT
}

using MaskFn = std::int32_t(std::int32_t, const void*, void*);

// Calls fn(how, set, old_set) with the stack shape of libc.prx's longjmp (0x2fd00): two pushes,
// then the call, so the callee starts with rsp % 16 == 0 instead of the SysV 8.
extern "C" std::int32_t call_like_longjmp(std::int32_t how, const void* set, void* old_set,
                                          MaskFn* fn);
asm(R"(
    .text
    .p2align 4
    .type call_like_longjmp, @function
call_like_longjmp:
    push %rdi
    push %rsi
    call *%rcx
    pop %rsi
    pop %rdi
    ret
    .size call_like_longjmp, .-call_like_longjmp
)");

TEST(KernelProcess, JmpSignalMaskSurvivesTheMisalignedStackOfLongjmp) {
    auto* mask = function<MaskFn>("6xVpy0Fdq+I");
    unsigned char saved[16];
    std::memset(saved, 0xcc, sizeof saved);
    testing::internal::CaptureStderr();
    EXPECT_EQ(call_like_longjmp(3, saved, nullptr, mask), 0);  // logs: within the first 10 calls
    EXPECT_EQ(call_like_longjmp(1, nullptr, saved, mask), 0);
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_NE(log.find("6xVpy0Fdq+I(3,"), std::string::npos) << log;
    for (unsigned char byte : saved) {
        EXPECT_EQ(byte, 0);
    }
}

TEST(KernelProcess, SignalCallsLeaveTheHostSignalStateAlone) {
    sigset_t before;
    ASSERT_EQ(pthread_sigmask(SIG_SETMASK, nullptr, &before), 0);
    struct sigaction action_before {};
    ASSERT_EQ(sigaction(SIGUSR1, nullptr, &action_before), 0);

    unsigned char set[16];
    std::memset(set, 0, sizeof set);
    EXPECT_EQ(function<std::int32_t(void*)>("VkTAsrZDcJ0")(set), 0);
    for (unsigned char byte : set) {
        EXPECT_EQ(byte, 0xff);
    }
    unsigned char old[16];
    std::memset(old, 0xcc, sizeof old);
    EXPECT_EQ(function<MaskFn>("aPcyptbOiZs")(3, set, old), 0);
    for (unsigned char byte : old) {
        EXPECT_EQ(byte, 0);
    }
    std::memset(old, 0xcc, sizeof old);
    EXPECT_EQ(function<MaskFn>("6xVpy0Fdq+I")(1, nullptr, old), 0);
    EXPECT_EQ(old[0], 0);
    EXPECT_EQ(function<MaskFn>("6xVpy0Fdq+I")(3, old, nullptr), 0);
    using SignalFn = std::uintptr_t(std::int32_t, std::uintptr_t);
    EXPECT_EQ(function<SignalFn>("VADc3MNQ3cM")(30, 0x1234), 0U);  // SIGUSR1 in FreeBSD
    EXPECT_EQ(function<std::int32_t(const void*)>("mo0bFmWppIw")(set), 0);

    sigset_t after;
    ASSERT_EQ(pthread_sigmask(SIG_SETMASK, nullptr, &after), 0);
    for (int signal = 1; signal < NSIG; ++signal) {
        EXPECT_EQ(sigismember(&after, signal), sigismember(&before, signal)) << signal;
    }
    struct sigaction action_after {};
    ASSERT_EQ(sigaction(SIGUSR1, nullptr, &action_after), 0);
    EXPECT_EQ(action_after.sa_handler, action_before.sa_handler);
}

using ReportFn = void(std::uint32_t, std::uint64_t);

TEST(KernelProcess, StatusReportIsLoggedAndReturns) {
    testing::internal::CaptureStderr();
    function<ReportFn>("zE-wXIZjLoM")(0xa0020003, 0);
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_NE(log.find("status report 0xa0020003"), std::string::npos) << log;
}

TEST(KernelProcessDeathTest, FatalStatusReportAborts) {
    auto* report = function<ReportFn>("OMDRKKAZ8I4");
    EXPECT_DEATH(report(0xa002000b, 0), "fatal status report 0xa002000b");
}

TEST(KernelProcessDeathTest, ExitEndsWithTheStatus) {
    auto* exit = function<void(std::int32_t)>("6Z83sYWFlA8");
    EXPECT_EXIT(exit(42), testing::ExitedWithCode(42), "_exit\\(42\\)");
    EXPECT_EXIT(exit(0), testing::ExitedWithCode(0), "");
}

}  // namespace
