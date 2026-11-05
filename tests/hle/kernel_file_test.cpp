#include "hle/kernel/file.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <memory>

#include <gtest/gtest.h>

namespace {

using namespace bb::hle::kernel;

TEST(FdTable, NewDescriptorsGetTheLowestFreeNumber) {
    FdTable table;
    const auto null = [] { return ::open("/dev/null", O_RDONLY | O_CLOEXEC); };
    for (int n = 0; n < 3; ++n) {
        EXPECT_EQ(table.insert(null()), n);
    }
    const std::shared_ptr<OpenFile> held = table.get(1);
    EXPECT_TRUE(table.remove(1));
    EXPECT_FALSE(table.remove(1));
    EXPECT_NE(::fcntl(held->host_fd, F_GETFD), -1);  // a call holding it keeps it open
    EXPECT_EQ(table.insert(null()), 1);
    EXPECT_EQ(table.insert(null()), 3);
    EXPECT_FALSE(table.remove(-1));
}

TEST(FdTable, AClosedHostStreamBecomesDevNullAboveTwo) {
    const int saved = ::fcntl(0, F_DUPFD_CLOEXEC, 3);
    ASSERT_GE(saved, 3);
    ASSERT_EQ(::close(0), 0);  // host 0 is now the lowest free number
    const int copy = copy_host_stream(0);
    const bool zero_free = ::fcntl(0, F_GETFD) == -1;
    ASSERT_EQ(::dup2(saved, 0), 0);
    ::close(saved);
    ASSERT_GE(copy, 3);
    EXPECT_TRUE(zero_free);
    struct stat st {}, null {};
    ASSERT_EQ(::fstat(copy, &st), 0);
    ASSERT_EQ(::stat("/dev/null", &null), 0);
    EXPECT_EQ(st.st_rdev, null.st_rdev);
    EXPECT_NE(::fcntl(copy, F_GETFD) & FD_CLOEXEC, 0);
    ::close(copy);
}

TEST(FdTable, GuestStdioDuplicatesTheHostStreams) {
    for (int n = 0; n < 3; ++n) {
        const std::shared_ptr<OpenFile> file = fd_table().get(n);
        ASSERT_NE(file, nullptr) << n;
        EXPECT_GE(file->host_fd, 3) << n;
        struct stat guest {}, host {};
        if (::fstat(n, &host) == 0) {  // a closed host stream is /dev/null for the guest
            ASSERT_EQ(::fstat(file->host_fd, &guest), 0);
            EXPECT_EQ(guest.st_dev, host.st_dev) << n;
            EXPECT_EQ(guest.st_ino, host.st_ino) << n;
        }
    }
    // Closing guest 2 leaves the host's stderr open.
    EXPECT_TRUE(fd_table().remove(2));
    EXPECT_NE(::fcntl(2, F_GETFD), -1);
    // Guest 2 is the lowest free number now.
    EXPECT_EQ(fd_table().insert(::open("/dev/null", O_RDONLY | O_CLOEXEC)), 2);
    EXPECT_TRUE(fd_table().remove(2));
    // Restore guest 2 for the other tests.
    EXPECT_EQ(fd_table().insert(::fcntl(2, F_DUPFD_CLOEXEC, 3)), 2);
}

}  // namespace
