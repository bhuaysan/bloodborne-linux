#include "hle/kernel/dir.hpp"

#include <stdlib.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "hle/kernel/abi.hpp"
#include "hle/kernel/file.hpp"
#include "hle/kernel/paths.hpp"

namespace {

namespace fs = std::filesystem;
using namespace bb::hle::kernel;

// Guest open flags (docs/formats/kernel-abi.md).
constexpr int o_rdonly = 0, o_rdwr = 2, o_directory = 0x20000;
// Guest d_type values.
constexpr int dt_dir = 4, dt_reg = 8;

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

struct Record {
    std::uint32_t fileno;
    std::uint16_t reclen;
    std::uint8_t type;
    std::string name;
};

// Parses the records that a getdirentries call wrote to `bytes`.
std::vector<Record> parse(const std::byte* bytes, std::size_t length) {
    std::vector<Record> out;
    for (std::size_t at = 0; at < length;) {
        GuestDirent d;
        std::memcpy(&d, bytes + at, 8);
        std::string name(reinterpret_cast<const char*>(bytes + at + 8), d.namlen);
        EXPECT_EQ(d.reclen, dirent_length(d.namlen));
        EXPECT_EQ(bytes[at + 8 + d.namlen], std::byte{0});
        out.push_back({d.fileno, d.reclen, d.type, std::move(name)});
        at += d.reclen;
    }
    return out;
}

// Every record of directory `fd`, read with buffers of `size` bytes at an odd address.
std::vector<Record> list_all(std::int32_t fd, std::int32_t size) {
    std::vector<std::byte> buffer(static_cast<std::size_t>(size) + 1);
    std::vector<Record> all;
    for (int calls = 0; calls < 10000; ++calls) {
        std::int64_t base = -1;
        const std::int32_t n = sceKernelGetdirentries(fd, buffer.data() + 1, size, &base);
        EXPECT_GE(n, 0);
        EXPECT_GE(base, 0);
        if (n <= 0) {
            break;
        }
        for (Record& r : parse(buffer.data() + 1, static_cast<std::size_t>(n))) {
            all.push_back(std::move(r));
        }
    }
    return all;
}

TEST_F(DirTest, ListsMoreEntriesThanOneBufferHoldsAndHidesJunk) {
    std::map<std::string, int> expected{{".", 0}, {"..", 0}, {"sub", 0}};
    for (int i = 0; i < 200; ++i) {
        const std::string name = "file_with_a_longer_name_" + std::to_string(i) + ".bin";
        write(save0_ / name, "x");
        expected[name] = 0;
    }
    fs::create_directory(save0_ / "sub");
    write(save0_ / "._file_0.bin", "junk");
    write(save0_ / ".DS_Store", "junk");

    const std::int32_t fd = sceKernelOpen("/savedata0", o_rdonly | o_directory, 0);
    ASSERT_GE(fd, 3);
    const std::vector<Record> all = list_all(fd, 100);  // at most two records per call
    std::map<std::string, int> seen;
    for (const Record& r : all) {
        ++seen[r.name];
        EXPECT_NE(r.fileno, 0u) << r.name;
        EXPECT_EQ(r.type, r.name == "sub" || r.name.starts_with(".") ? dt_dir : dt_reg) << r.name;
    }
    for (auto& [name, count] : expected) {
        count = 1;
    }
    EXPECT_EQ(seen, expected);
    EXPECT_EQ(sceKernelClose(fd), 0);
}

TEST_F(DirTest, SeekingBackToTheBaseListsTheSameRecords) {
    for (int i = 0; i < 20; ++i) {
        write(save0_ / ("f" + std::to_string(i)), "");
    }
    const std::int32_t fd = sceKernelOpen("/savedata0", o_rdonly | o_directory, 0);
    ASSERT_GE(fd, 3);
    std::byte first[64], again[64];
    std::int64_t base0 = -1, base1 = -1, base2 = -1;
    ASSERT_GT(sceKernelGetdirentries(fd, first, sizeof first, &base0), 0);
    const std::int32_t n = sceKernelGetdirentries(fd, first, sizeof first, &base1);
    ASSERT_GT(n, 0);
    EXPECT_NE(base1, base0);
    EXPECT_EQ(sceKernelLseek(fd, base1, 0), base1);
    ASSERT_EQ(sceKernelGetdirentries(fd, again, sizeof again, &base2), n);
    EXPECT_EQ(base2, base1);
    EXPECT_EQ(std::memcmp(first, again, static_cast<std::size_t>(n)), 0);
    // A null base is allowed (libSceFios2 0x2d425).
    EXPECT_GT(sceKernelGetdirentries(fd, again, sizeof again, nullptr), 0);
    EXPECT_EQ(sceKernelClose(fd), 0);
}

TEST_F(DirTest, RefusesBuffersTooSmallForTheNextRecordAndNonDirectories) {
    write(save0_ / "a_name_of_twenty_two_b", "");
    const std::int32_t fd = sceKernelOpen("/savedata0", o_rdonly | o_directory, 0);
    ASSERT_GE(fd, 3);
    std::byte buffer[512];
    EXPECT_EQ(sceKernelGetdirentries(fd, buffer, 8, nullptr), sce(EINVAL));
    EXPECT_EQ(sceKernelGetdirentries(fd, buffer, -1, nullptr), sce(EINVAL));
    EXPECT_EQ(sceKernelGetdirentries(fd, nullptr, 512, nullptr), sce(EFAULT));
    // The refused calls didn't move the read position.
    std::map<std::string, int> seen;
    for (const Record& r : list_all(fd, sizeof buffer)) {
        ++seen[r.name];
    }
    EXPECT_EQ(seen.size(), 3u);
    EXPECT_EQ(seen["a_name_of_twenty_two_b"], 1);
    EXPECT_EQ(sceKernelClose(fd), 0);

    const std::int32_t file = sceKernelOpen("/app0/dvdroot_ps4/a.txt", o_rdonly, 0);
    ASSERT_GE(file, 3);
    EXPECT_EQ(sceKernelGetdirentries(file, buffer, sizeof buffer, nullptr), sce(EINVAL));
    EXPECT_EQ(sceKernelClose(file), 0);
    EXPECT_EQ(sceKernelGetdirentries(1000, buffer, sizeof buffer, nullptr), sce(EBADF));

    // A descriptor that can't seek, like a host stream at guest 0-2.
    int ends[2];
    ASSERT_EQ(::pipe(ends), 0);
    const std::int32_t pipe = fd_table().insert(ends[0]);
    EXPECT_EQ(sceKernelGetdirentries(pipe, buffer, sizeof buffer, nullptr), sce(EINVAL));
    EXPECT_EQ(sceKernelClose(pipe), 0);
    ::close(ends[1]);
}

TEST_F(DirTest, ListsTheGameDirectory) {
    write(game_ / "dvdroot_ps4" / "._a.txt", "junk");
    const std::int32_t fd = sceKernelOpen("/app0/dvdroot_ps4", o_rdonly | o_directory, 0);
    ASSERT_GE(fd, 3);
    std::map<std::string, int> seen;
    for (const Record& r : list_all(fd, 512)) {
        ++seen[r.name];
    }
    EXPECT_EQ(seen, (std::map<std::string, int>{{".", 1}, {"..", 1}, {"a.txt", 1}}));
    EXPECT_EQ(sceKernelClose(fd), 0);
}

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
