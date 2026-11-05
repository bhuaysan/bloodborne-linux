#include "hle/kernel/abi.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <set>
#include <thread>

#include <gtest/gtest.h>

namespace {

using namespace bb::hle::kernel;

struct Value {
    int guest, host;
};

TEST(KernelAbi, EveryGuestErrnoRoundTripsOrHasNoHostError) {
    const std::set<int> no_host = {67, 72, 73, 74, 75, 76, 79, 80, 81, 88, 93, 94};
    for (int guest = 1; guest <= guest_elast; ++guest) {
        const auto host = host_errno(guest);
        EXPECT_EQ(host.has_value(), !no_host.contains(guest)) << guest;
        if (host) {
            EXPECT_EQ(guest_errno(*host), guest) << guest;
        }
    }
    EXPECT_FALSE(host_errno(0));
    EXPECT_FALSE(host_errno(guest_elast + 1));
    EXPECT_FALSE(host_errno(-1));
}

TEST(KernelAbi, ErrnoValuesThatDifferFromLinux) {
    const Value values[] = {{11, EDEADLK},      {35, EAGAIN},       {35, EWOULDBLOCK},
                            {36, EINPROGRESS},  {45, EOPNOTSUPP},   {45, ENOTSUP},
                            {60, ETIMEDOUT},    {62, ELOOP},        {63, ENAMETOOLONG},
                            {66, ENOTEMPTY},    {77, ENOLCK},       {78, ENOSYS},
                            {84, EOVERFLOW},    {86, EILSEQ},       {87, ENODATA},
                            {96, EOWNERDEAD}};
    for (const Value& v : values) {
        EXPECT_EQ(guest_errno(v.host), v.guest) << v.host;
    }
}

TEST(KernelAbi, HostOnlyErrnoIsReported) {
    EXPECT_FALSE(guest_errno(ENOMEDIUM));
    EXPECT_FALSE(guest_errno(EKEYEXPIRED));
    EXPECT_FALSE(guest_errno(0));
}

TEST(KernelAbi, SceErrorEncoding) {
    EXPECT_EQ(sce_error(2), static_cast<std::int32_t>(0x80020002u));
    for (int guest = 1; guest <= guest_elast; ++guest) {
        EXPECT_LT(sce_error(guest), 0);
        EXPECT_EQ(errno_from_sce(sce_error(guest)), guest);
    }
    // A code of another facility, e.g. one of libSceFios2, becomes EIO in libc.prx.
    EXPECT_EQ(errno_from_sce(static_cast<std::int32_t>(0x80820002u)), 5);
}

TEST(KernelAbi, PosixFamilySetsGuestErrno) {
    *guest_errno_location() = 0;
    EXPECT_EQ(posix_fail(ENOENT), -1);
    EXPECT_EQ(*guest_errno_location(), 2);
    EXPECT_EQ(posix_fail(EAGAIN), -1);
    EXPECT_EQ(*guest_errno_location(), 35);
    EXPECT_EQ(posix_fail(ENOMEDIUM), -1);
    EXPECT_EQ(*guest_errno_location(), 5);
}

TEST(KernelAbi, PthreadAndSceFamiliesLeaveGuestErrnoAlone) {
    *guest_errno_location() = 1234;
    EXPECT_EQ(pthread_error(0), 0);
    EXPECT_EQ(pthread_error(EBUSY), 16);
    EXPECT_EQ(pthread_error(ETIMEDOUT), 60);
    EXPECT_EQ(pthread_error(EDEADLK), 11);
    EXPECT_EQ(sce_fail(0), 0);
    EXPECT_EQ(sce_fail(ENOENT), sce_error(2));
    EXPECT_EQ(sce_fail(ENOMEDIUM), sce_error(5));
    EXPECT_EQ(*guest_errno_location(), 1234);
}

TEST(KernelAbi, GuestErrnoIsPerThread) {
    int* mine = guest_errno_location();
    EXPECT_EQ(guest_errno_location(), mine);
    *mine = 7;
    int* theirs = nullptr;
    int initial = -1;
    std::thread([&] {
        theirs = guest_errno_location();
        initial = *theirs;
        posix_fail(EINVAL);
    }).join();
    EXPECT_NE(theirs, mine);
    EXPECT_EQ(initial, 0);
    EXPECT_EQ(*mine, 7);
}

TEST(KernelAbi, OpenFlagsRoundTrip) {
    const Value values[] = {
        {0x0, O_RDONLY},    {0x1, O_WRONLY},       {0x2, O_RDWR},        {0x4, O_NONBLOCK},
        {0x8, O_APPEND},    {0x40, O_ASYNC},       {0x80, O_SYNC},       {0x100, O_NOFOLLOW},
        {0x200, O_CREAT},   {0x400, O_TRUNC},      {0x800, O_EXCL},      {0x8000, O_NOCTTY},
        {0x10000, O_DIRECT}, {0x20000, O_DIRECTORY}, {0x100000, O_CLOEXEC},
    };
    for (const Value& v : values) {
        EXPECT_EQ(host_open_flags(v.guest), (Flags{v.host, 0})) << std::hex << v.guest;
        EXPECT_EQ(guest_open_flags(v.host), (Flags{v.guest, 0})) << std::hex << v.host;
    }
    // libc.prx's fopen "w" and the directory open of libSceFios2.
    EXPECT_EQ(host_open_flags(0x601), (Flags{O_WRONLY | O_CREAT | O_TRUNC, 0}));
    EXPECT_EQ(host_open_flags(0x20004), (Flags{O_DIRECTORY | O_NONBLOCK, 0}));
}

TEST(KernelAbi, GuestOpenFlagsDropHostLargeFile) {
    // The kernel's __O_LARGEFILE (0x8000), which F_GETFL always reports on x86-64.
    EXPECT_EQ(guest_open_flags(0x8000 | O_RDWR), (Flags{0x2, 0}));
    EXPECT_EQ(guest_open_flags(0x8000 | O_NOCTTY), (Flags{0x8000, 0}));

    const int fd = ::open("/dev/null", O_RDWR | O_CLOEXEC);
    ASSERT_GE(fd, 0);
    const int host = ::fcntl(fd, F_GETFL);
    ::close(fd);
    ASSERT_GE(host, 0);
    EXPECT_EQ(guest_open_flags(host), (Flags{0x2, 0})) << std::hex << host;
}

TEST(KernelAbi, UnknownOpenFlagsAreReported) {
    // O_SHLOCK, O_EXLOCK, O_EXEC, O_TTY_INIT, an unassigned bit, and access mode 3.
    for (int bit : {0x10, 0x20, 0x40000, 0x80000, 0x200000}) {
        EXPECT_EQ(host_open_flags(bit | 0x200), (Flags{O_CREAT, bit})) << std::hex << bit;
    }
    EXPECT_EQ(host_open_flags(0x3), (Flags{0, 0x3}));
    EXPECT_EQ(guest_open_flags(O_RDWR | O_DSYNC), (Flags{0x2, O_DSYNC}));
}

TEST(KernelAbi, ProtRoundTripsAndReportsHigherBits) {
    const Value values[] = {{0x1, PROT_READ}, {0x2, PROT_WRITE}, {0x4, PROT_EXEC}};
    for (const Value& v : values) {
        EXPECT_EQ(host_prot(v.guest), (Flags{v.host, 0}));
        EXPECT_EQ(guest_prot(v.host), (Flags{v.guest, 0}));
    }
    EXPECT_EQ(host_prot(0), (Flags{PROT_NONE, 0}));
    EXPECT_EQ(host_prot(0x33), (Flags{PROT_READ | PROT_WRITE, 0x30}));
}

TEST(KernelAbi, MapFlagsRoundTrip) {
    const Value values[] = {
        {0x1, MAP_SHARED}, {0x2, MAP_PRIVATE},      {0x10, MAP_FIXED},    {0x40, MAP_NORESERVE},
        {0x400, MAP_STACK}, {0x1000, MAP_ANONYMOUS}, {0x80000, MAP_32BIT},
    };
    for (const Value& v : values) {
        EXPECT_EQ(host_map_flags(v.guest), (Flags{v.host, 0})) << std::hex << v.guest;
        EXPECT_EQ(guest_map_flags(v.host), (Flags{v.guest, 0})) << std::hex << v.host;
    }
    // libc.prx's anonymous mappings.
    EXPECT_EQ(host_map_flags(0x1002), (Flags{MAP_ANONYMOUS | MAP_PRIVATE, 0}));
}

TEST(KernelAbi, UnknownMapFlagsAreReported) {
    // MAP_RENAME, MAP_HASSEMAPHORE, MAP_NOSYNC, 0x2000, MAP_EXCL, MAP_NOCORE, MAP_PREFAULT_READ
    // and an alignment request.
    for (int bit : {0x20, 0x200, 0x800, 0x2000, 0x4000, 0x20000, 0x40000, 0x0c000000}) {
        EXPECT_EQ(host_map_flags(bit | 0x2), (Flags{MAP_PRIVATE, bit})) << std::hex << bit;
    }
}

TEST(KernelAbi, Whence) {
    for (int whence = 0; whence <= 4; ++whence) {
        EXPECT_EQ(host_whence(whence), whence);
    }
    EXPECT_FALSE(host_whence(5));
    EXPECT_FALSE(host_whence(-1));
}

TEST(KernelAbi, ClockIds) {
    struct Clock {
        std::int32_t guest;
        clockid_t host;
    };
    const Clock values[] = {
        {0, CLOCK_REALTIME},  {4, CLOCK_MONOTONIC},           {5, CLOCK_MONOTONIC},
        {7, CLOCK_MONOTONIC}, {8, CLOCK_MONOTONIC},           {9, CLOCK_REALTIME},
        {10, CLOCK_REALTIME}, {11, CLOCK_MONOTONIC},          {12, CLOCK_MONOTONIC},
        {13, CLOCK_REALTIME}, {14, CLOCK_THREAD_CPUTIME_ID}, {15, CLOCK_PROCESS_CPUTIME_ID},
    };
    for (const Clock& c : values) {
        EXPECT_EQ(host_clock(c.guest), c.host) << c.guest;
    }
    for (std::int32_t unknown : {1, 2, 3, 6, 16, -1}) {
        EXPECT_FALSE(host_clock(unknown)) << unknown;
    }
}

}  // namespace
