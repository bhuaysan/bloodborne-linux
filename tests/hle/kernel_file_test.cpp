#include "hle/kernel/file.hpp"

#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "hle/kernel/abi.hpp"
#include "hle/kernel/paths.hpp"
#include "support/game.hpp"

namespace {

namespace fs = std::filesystem;
using namespace bb::hle::kernel;

// Guest open flags and whence (docs/formats/kernel-abi.md).
constexpr int o_rdonly = 0, o_wronly = 1, o_rdwr = 2, o_creat = 0x200, o_trunc = 0x400;
constexpr int o_direct = 0x10000, o_exec = 0x40000;
constexpr int seek_set = 0, seek_end = 2;

std::int32_t sce(int host_errno) {
    return sce_error(guest_errno(host_errno).value());
}

std::string slurp(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

// A temporary game directory mounted at /app0 and a save data directory at /savedata0.
class FileTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::string templ = (fs::temp_directory_path() / "bb-file-test-XXXXXX").string();
        ASSERT_NE(::mkdtemp(templ.data()), nullptr);
        tmp_ = templ;
        game_ = tmp_ / "game";
        save_ = tmp_ / "save";
        fs::create_directories(game_ / "dvdroot_ps4");
        fs::create_directories(save_);
        std::ofstream(game_ / "dvdroot_ps4" / "a.txt") << "hello world";
        path_map().mount_game(game_);
        ASSERT_TRUE(path_map().mount("/savedata0", save_));
    }
    void TearDown() override {
        path_map().unmount("/savedata0");
        fs::remove_all(tmp_);
    }

    fs::path tmp_, game_, save_;
};

TEST_F(FileTest, ReadsButNeverWritesUnderTheGameMount) {
    const std::int32_t fd = sceKernelOpen("/app0/dvdroot_ps4/a.txt", o_rdonly, 0);
    ASSERT_GE(fd, 3);
    char buffer[16] = {};
    EXPECT_EQ(sceKernelRead(fd, buffer, 5), 5);
    EXPECT_EQ(std::string(buffer, 5), "hello");
    EXPECT_EQ(sceKernelPread(fd, buffer, sizeof buffer, 6), 5);
    EXPECT_EQ(std::string(buffer, 5), "world");
    EXPECT_EQ(sceKernelLseek(fd, 0, 1), 5);  // pread left the offset alone
    EXPECT_EQ(sceKernelClose(fd), 0);
    EXPECT_EQ(sceKernelOpen("/app0/dvdroot_ps4/missing", o_rdonly, 0), sce(ENOENT));
    EXPECT_EQ(sceKernelOpen("/app0/dvdroot_ps4/a.txt", o_wronly, 0), sce(EROFS));
    EXPECT_EQ(sceKernelOpen("/app0/dvdroot_ps4/a.txt", o_rdonly | o_trunc, 0), sce(EROFS));
    EXPECT_EQ(sceKernelOpen("/app0/new.txt", o_rdwr | o_creat, 0666), sce(EROFS));
    EXPECT_EQ(slurp(game_ / "dvdroot_ps4" / "a.txt"), "hello world");
}

TEST_F(FileTest, WritesReadsAndSeeksInSaveData) {
    const std::int32_t fd = sceKernelOpen("/savedata0/s.bin", o_rdwr | o_creat | o_trunc, 0666);
    ASSERT_GE(fd, 3);
    EXPECT_EQ(sceKernelWrite(fd, "abcdef", 6), 6);
    EXPECT_EQ(sceKernelPwrite(fd, "XY", 2, 1), 2);
    EXPECT_EQ(sceKernelFsync(fd), 0);
    EXPECT_EQ(sceKernelLseek(fd, 0, seek_set), 0);
    char buffer[8] = {};
    EXPECT_EQ(sceKernelRead(fd, buffer, sizeof buffer), 6);
    EXPECT_EQ(std::string(buffer, 6), "aXYdef");
    EXPECT_EQ(slurp(save_ / "s.bin"), "aXYdef");
    // Past the end: reads give nothing, a write leaves a hole.
    EXPECT_EQ(sceKernelLseek(fd, 100, seek_set), 100);
    EXPECT_EQ(sceKernelRead(fd, buffer, 1), 0);
    EXPECT_EQ(sceKernelWrite(fd, "z", 1), 1);
    EXPECT_EQ(sceKernelLseek(fd, 0, seek_end), 101);
    EXPECT_EQ(sceKernelLseek(fd, -1, seek_set), sce(EINVAL));
    EXPECT_EQ(sceKernelLseek(fd, 0, 7), sce(EINVAL));
    EXPECT_EQ(sceKernelClose(fd), 0);
    EXPECT_EQ(fs::file_size(save_ / "s.bin"), 101u);
}

TEST_F(FileTest, StatFillsTheGuestLayout) {
    struct stat host {};
    ASSERT_EQ(::stat((game_ / "dvdroot_ps4" / "a.txt").c_str(), &host), 0);
    const char* const path = "/app0/dvdroot_ps4/a.txt";
    const std::int32_t fd = sceKernelOpen(path, o_rdonly, 0);
    ASSERT_GE(fd, 0);
    GuestStat s[4] = {};
    EXPECT_EQ(sceKernelStat(path, &s[0]), 0);
    EXPECT_EQ(posix_stat(path, &s[1]), 0);
    EXPECT_EQ(sceKernelFstat(fd, &s[2]), 0);
    EXPECT_EQ(guest_fstat(fd, &s[3]), 0);
    EXPECT_EQ(sceKernelClose(fd), 0);
    for (const GuestStat& one : s) {
        EXPECT_EQ(std::memcmp(&one, &s[0], sizeof one), 0);
    }
    EXPECT_EQ(s[0].st_size, 11);
    EXPECT_EQ(s[0].st_mode & 0170000, 0100000);  // S_IFREG
    EXPECT_EQ(s[0].st_ino, static_cast<std::uint32_t>(host.st_ino));
    EXPECT_EQ(s[0].st_mtim.tv_nsec, host.st_mtim.tv_nsec);

    GuestStat missing{};
    *guest_errno_location() = 0;
    EXPECT_EQ(posix_stat("/app0/missing", &missing), -1);
    EXPECT_EQ(*guest_errno_location(), guest_errno(ENOENT).value());
    EXPECT_EQ(sceKernelStat("/hostapp/x", &missing), sce(ENOENT));
    EXPECT_EQ(sceKernelStat(nullptr, &missing), sce(EFAULT));
}

TEST_F(FileTest, ClosedDescriptorsFailWithEbadf) {
    EXPECT_EQ(sceKernelClose(1000), sce(EBADF));
    EXPECT_EQ(sceKernelClose(-1), sce(EBADF));
    const std::int32_t fd = sceKernelOpen("/app0/dvdroot_ps4/a.txt", o_rdonly, 0);
    ASSERT_GE(fd, 0);
    EXPECT_EQ(sceKernelClose(fd), 0);
    EXPECT_EQ(sceKernelClose(fd), sce(EBADF));
    char c = 0;
    EXPECT_EQ(sceKernelRead(fd, &c, 1), sce(EBADF));
    GuestStat s{};
    *guest_errno_location() = 0;
    EXPECT_EQ(guest_fstat(fd, &s), -1);
    EXPECT_EQ(*guest_errno_location(), guest_errno(EBADF).value());
}

TEST_F(FileTest, DropsODirectAndRefusesUnknownFlags) {
    const std::int32_t fd = sceKernelOpen("/app0/dvdroot_ps4/a.txt", o_rdonly | o_direct, 0);
    ASSERT_GE(fd, 0);
    char buffer[8] = {};
    EXPECT_EQ(sceKernelPread(fd, buffer + 1, 3, 1), 3);  // unaligned buffer, size and offset
    EXPECT_EQ(std::string(buffer + 1, 3), "ell");
    EXPECT_EQ(sceKernelClose(fd), 0);
    EXPECT_EQ(sceKernelOpen("/app0/dvdroot_ps4/a.txt", o_rdonly | o_exec, 0), sce(EINVAL));
    EXPECT_EQ(sceKernelOpen("/app0/dvdroot_ps4/a.txt", 3, 0), sce(EINVAL));
}

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

TEST_F(FileTest, GuestStdioDuplicatesTheHostStreams) {
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
    EXPECT_EQ(sceKernelClose(2), 0);
    EXPECT_NE(::fcntl(2, F_GETFD), -1);
    // Guest 2 is the lowest free number now.
    EXPECT_EQ(sceKernelOpen("/app0/dvdroot_ps4/a.txt", o_rdonly, 0), 2);
    EXPECT_EQ(sceKernelClose(2), 0);
    // Restore guest 2 for the other tests.
    EXPECT_EQ(fd_table().insert(::fcntl(2, F_DUPFD_CLOEXEC, 3)), 2);
}

TEST(FileGame, ReadsAShaderArchiveHeader) {
    BB_REQUIRE_GAME(game);
    path_map().mount_game(game);
    const std::int32_t fd =
        sceKernelOpen("/app0/dvdroot_ps4/shader/gxgui.shaderbnd.dcx", o_rdonly, 0);
    ASSERT_GE(fd, 0) << std::hex << fd;
    char magic[4] = {};
    EXPECT_EQ(sceKernelRead(fd, magic, sizeof magic), 4);
    EXPECT_EQ(std::memcmp(magic, "DCX\0", 4), 0);
    GuestStat s{};
    EXPECT_EQ(sceKernelFstat(fd, &s), 0);
    EXPECT_GT(s.st_size, 4);
    EXPECT_EQ(sceKernelClose(fd), 0);
}

}  // namespace
