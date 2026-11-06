#include "hle/kernel/dir.hpp"

#include <stdlib.h>

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include "hle/kernel/abi.hpp"
#include "hle/kernel/file.hpp"
#include "hle/kernel/paths.hpp"

namespace {

namespace fs = std::filesystem;
using namespace bb::hle::kernel;

// Guest open flags (docs/formats/kernel-abi.md).
constexpr int o_rdonly = 0, o_rdwr = 2;

std::int32_t sce(int host_errno) {
    return sce_error(guest_errno(host_errno).value());
}

// A game directory at /app0 and two save data directories at /savedata0 and /savedata1.
class DirTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::string templ = (fs::temp_directory_path() / "bb-dir-test-XXXXXX").string();
        ASSERT_NE(::mkdtemp(templ.data()), nullptr);
        tmp_ = templ;
        game_ = tmp_ / "game";
        save0_ = tmp_ / "save0";
        save1_ = tmp_ / "save1";
        fs::create_directories(game_ / "dvdroot_ps4");
        fs::create_directories(save0_);
        fs::create_directories(save1_);
        std::ofstream(game_ / "dvdroot_ps4" / "a.txt") << "game";
        path_map().mount_game(game_);
        ASSERT_TRUE(path_map().mount("/savedata0", save0_));
        ASSERT_TRUE(path_map().mount("/savedata1", save1_));
    }
    void TearDown() override {
        path_map().unmount("/savedata0");
        path_map().unmount("/savedata1");
        fs::remove_all(tmp_);
    }

    static void write(const fs::path& file, const std::string& text) {
        std::ofstream(file, std::ios::binary) << text;
    }

    fs::path tmp_, game_, save0_, save1_;
};

TEST_F(DirTest, CreatesAndRemovesDirectoriesAndFiles) {
    EXPECT_EQ(sceKernelMkdir("/savedata0/d", 0755), 0);
    EXPECT_TRUE(fs::is_directory(save0_ / "d"));
    EXPECT_EQ(sceKernelMkdir("/savedata0/d", 0755), sce(EEXIST));
    write(save0_ / "d" / "f", "x");
    EXPECT_EQ(sceKernelRmdir("/savedata0/d"), sce(ENOTEMPTY));
    EXPECT_EQ(sceKernelUnlink("/savedata0/d"), sce(EPERM));
    EXPECT_EQ(sceKernelUnlink("/savedata0/d/f"), 0);
    EXPECT_EQ(sceKernelUnlink("/savedata0/d/f"), sce(ENOENT));
    EXPECT_EQ(sceKernelRmdir("/savedata0/d"), 0);
    EXPECT_FALSE(fs::exists(save0_ / "d"));

    // The mount points themselves stay.
    EXPECT_EQ(sceKernelRmdir("/savedata0"), sce(EBUSY));
    EXPECT_EQ(sceKernelUnlink("/savedata0/"), sce(EBUSY));
    EXPECT_TRUE(fs::is_directory(save0_));

    EXPECT_EQ(sceKernelMkdir("/app0/new", 0755), sce(EROFS));
    EXPECT_EQ(sceKernelRmdir("/app0/dvdroot_ps4"), sce(EROFS));
    EXPECT_EQ(sceKernelUnlink("/app0/dvdroot_ps4/a.txt"), sce(EROFS));
    EXPECT_EQ(sceKernelMkdir("/nowhere/d", 0755), sce(ENOENT));
    EXPECT_EQ(sceKernelMkdir(nullptr, 0755), sce(EFAULT));
    EXPECT_TRUE(fs::exists(game_ / "dvdroot_ps4" / "a.txt"));
}

TEST_F(DirTest, PosixCallsSetTheGuestErrno) {
    EXPECT_EQ(posix_mkdir("/savedata0/p", 0700), 0);
    *guest_errno_location() = 0;
    EXPECT_EQ(posix_mkdir("/savedata0/p", 0700), -1);
    EXPECT_EQ(*guest_errno_location(), 17);  // EEXIST
    EXPECT_EQ(posix_rename("/savedata0/p", "/savedata0/q"), 0);
    EXPECT_TRUE(fs::is_directory(save0_ / "q"));
    EXPECT_EQ(posix_rename("/savedata0/q", "/savedata1/q"), -1);
    EXPECT_EQ(*guest_errno_location(), 18);  // EXDEV
    EXPECT_EQ(posix_rmdir("/savedata0/q"), 0);
    EXPECT_EQ(posix_rmdir("/app0/dvdroot_ps4"), -1);
    EXPECT_EQ(*guest_errno_location(), 30);  // EROFS
}

TEST_F(DirTest, RenamesWithinOneMountOnly) {
    fs::create_directories(save0_ / "a");
    fs::create_directories(save0_ / "b");
    write(save0_ / "a" / "f", "data");
    EXPECT_EQ(sceKernelRename("/savedata0/a/f", "/savedata0/b/g"), 0);
    EXPECT_FALSE(fs::exists(save0_ / "a" / "f"));
    EXPECT_TRUE(fs::exists(save0_ / "b" / "g"));
    EXPECT_EQ(sceKernelRename("/savedata0/b", "/savedata0/a/b"), 0);
    EXPECT_TRUE(fs::exists(save0_ / "a" / "b" / "g"));
    EXPECT_EQ(sceKernelRename("/savedata0/missing", "/savedata0/x"), sce(ENOENT));

    // Across mounts, even though both lie on one host file system.
    EXPECT_EQ(sceKernelRename("/savedata0/a/b/g", "/savedata1/g"), sce(EXDEV));
    EXPECT_TRUE(fs::exists(save0_ / "a" / "b" / "g"));
    EXPECT_FALSE(fs::exists(save1_ / "g"));
    // Under /app0, from it and onto it.
    EXPECT_EQ(sceKernelRename("/app0/dvdroot_ps4/a.txt", "/app0/dvdroot_ps4/b.txt"), sce(EROFS));
    EXPECT_EQ(sceKernelRename("/app0/dvdroot_ps4/a.txt", "/savedata0/a.txt"), sce(EROFS));
    EXPECT_EQ(sceKernelRename("/savedata0/a/b/g", "/app0/g"), sce(EROFS));
    EXPECT_TRUE(fs::exists(game_ / "dvdroot_ps4" / "a.txt"));
    // Mount points can't move, and nothing can replace them.
    EXPECT_EQ(sceKernelRename("/savedata0", "/savedata0/x"), sce(EBUSY));
    fs::create_directories(save0_ / "empty");
    EXPECT_EQ(sceKernelRename("/savedata0/empty", "/savedata0/"), sce(EBUSY));
    EXPECT_TRUE(fs::is_directory(save0_ / "empty"));
}

TEST_F(DirTest, TruncatesBothWays) {
    write(save0_ / "t", "0123456789");
    EXPECT_EQ(sceKernelTruncate("/savedata0/t", 4), 0);
    EXPECT_EQ(fs::file_size(save0_ / "t"), 4u);
    EXPECT_EQ(sceKernelTruncate("/savedata0/t", 16), 0);
    EXPECT_EQ(fs::file_size(save0_ / "t"), 16u);
    EXPECT_EQ(sceKernelTruncate("/savedata0/t", -1), sce(EINVAL));
    EXPECT_EQ(sceKernelTruncate("/savedata0/missing", 0), sce(ENOENT));
    EXPECT_EQ(sceKernelTruncate("/app0/dvdroot_ps4/a.txt", 0), sce(EROFS));

    const std::int32_t fd = sceKernelOpen("/savedata0/t", o_rdwr, 0);
    ASSERT_GE(fd, 3);
    EXPECT_EQ(sceKernelFtruncate(fd, 2), 0);
    EXPECT_EQ(fs::file_size(save0_ / "t"), 2u);
    EXPECT_EQ(sceKernelFtruncate(fd, 4096), 0);
    EXPECT_EQ(fs::file_size(save0_ / "t"), 4096u);
    char head[4] = {1, 1, 1, 1};
    EXPECT_EQ(sceKernelPread(fd, head, sizeof head, 0), 4);
    EXPECT_EQ(std::string(head, 4), std::string("01\0\0", 4));  // the new bytes read as zeros
    EXPECT_EQ(sceKernelClose(fd), 0);
    EXPECT_EQ(sceKernelFtruncate(fd, 0), sce(EBADF));

    // A descriptor under /app0 is read-only.
    const std::int32_t game = sceKernelOpen("/app0/dvdroot_ps4/a.txt", o_rdonly, 0);
    ASSERT_GE(game, 3);
    EXPECT_EQ(sceKernelFtruncate(game, 0), sce(EINVAL));
    EXPECT_EQ(sceKernelClose(game), 0);
    EXPECT_EQ(fs::file_size(game_ / "dvdroot_ps4" / "a.txt"), 4u);
}

}  // namespace
