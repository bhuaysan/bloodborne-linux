// File mappings of guest descriptors (issue #164): guest mmap without MAP_ANON.
#include <stdlib.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <gtest/gtest.h>

#include "hle/kernel/abi.hpp"
#include "hle/kernel/file.hpp"
#include "hle/kernel/paths.hpp"
#include "hle/kernel/vmem.hpp"

namespace {

namespace fs = std::filesystem;
using namespace bb::hle::kernel;
using vmem::page_size;

// Guest values (docs/formats/kernel-abi.md).
constexpr int prot_read = 0x1, prot_write = 0x2;
constexpr int map_shared = 0x1, map_private = 0x2, map_anon = 0x1000;
constexpr int o_rdonly = 0;
void* const map_failed = reinterpret_cast<void*>(-1);

// The guest errno after a POSIX call; resets it.
int take_errno() {
    const int e = *guest_errno_location();
    *guest_errno_location() = 0;
    return e;
}

int guest(int host_errno) {
    return guest_errno(host_errno).value();
}

std::string slurp(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

// A temporary game directory mounted at /app0 with a file of two guest pages and a bit: page n
// is filled with the letter 'a' + n.
class MmapFileTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::string templ = (fs::temp_directory_path() / "bb-mmap-test-XXXXXX").string();
        ASSERT_NE(::mkdtemp(templ.data()), nullptr);
        game_ = templ;
        contents_ = std::string(page_size, 'a') + std::string(page_size, 'b') + "cc";
        std::ofstream(game_ / "data.bin", std::ios::binary) << contents_;
        path_map().mount_game(game_);
        fd_ = sceKernelOpen("/app0/data.bin", o_rdonly, 0);
        ASSERT_GE(fd_, 3);
    }
    void TearDown() override {
        sceKernelClose(fd_);
        fs::remove_all(game_);
    }

    fs::path game_;
    std::string contents_;
    std::int32_t fd_ = -1;
};

TEST_F(MmapFileTest, MapsAFileReadOnlyAndRecordsTheRegion) {
    // As libc.prx 0x530e1 does it: the file size, unrounded, at offset 0.
    void* const p = guest_mmap(nullptr, contents_.size(), prot_read, map_private, fd_, 0);
    ASSERT_NE(p, map_failed);
    const auto at = reinterpret_cast<std::uintptr_t>(p);
    EXPECT_EQ(at % page_size, 0U);
    EXPECT_EQ(std::string(static_cast<const char*>(p), contents_.size()), contents_);

    const auto all = vmem::regions();
    const auto region = std::find_if(all.begin(), all.end(),
                                     [&](const vmem::Region& r) { return r.start == at; });
    ASSERT_NE(region, all.end());
    EXPECT_EQ(region->end, at + 3 * page_size);
    EXPECT_EQ(region->prot, prot_read);
    EXPECT_EQ(region->flags, 0U);
    EXPECT_EQ(region->name, "fd " + std::to_string(fd_));

    // The mapping outlives the guest descriptor.
    ASSERT_EQ(sceKernelClose(fd_), 0);
    fd_ = -1;
    EXPECT_EQ(static_cast<const char*>(p)[2 * page_size], 'c');
    EXPECT_EQ(guest_munmap(p, contents_.size()), 0);
    EXPECT_FALSE(vmem::find(at));
}

TEST_F(MmapFileTest, MapsAtAPageOffset) {
    void* const p = guest_mmap(nullptr, page_size, prot_read, map_private, fd_, page_size);
    ASSERT_NE(p, map_failed);
    EXPECT_EQ(static_cast<const char*>(p)[0], 'b');
    EXPECT_EQ(static_cast<const char*>(p)[page_size - 1], 'b');
    EXPECT_EQ(guest_munmap(p, page_size), 0);
}

TEST_F(MmapFileTest, NeverWritesToAReadOnlyDescriptor) {
    // Shared and writable needs a descriptor open for writing, as on FreeBSD (mmap(2), EACCES).
    EXPECT_EQ(guest_mmap(nullptr, page_size, prot_read | prot_write, map_shared, fd_, 0),
              map_failed);
    EXPECT_EQ(take_errno(), guest(EACCES));

    // A private writable mapping is a copy: the file keeps its bytes.
    for (const int sharing : {map_private, 0}) {
        auto* const p = static_cast<char*>(
            guest_mmap(nullptr, page_size, prot_read | prot_write, sharing, fd_, 0));
        ASSERT_NE(static_cast<void*>(p), map_failed);
        p[0] = 'z';
        EXPECT_EQ(guest_munmap(p, page_size), 0);
    }
    EXPECT_EQ(slurp(game_ / "data.bin"), contents_);
}

TEST_F(MmapFileTest, RefusesUnknownDescriptorsAndUnalignedOffsets) {
    EXPECT_EQ(guest_mmap(nullptr, page_size, prot_read, map_private, 999, 0), map_failed);
    EXPECT_EQ(take_errno(), guest(EBADF));
    EXPECT_EQ(guest_mmap(nullptr, page_size, prot_read, map_private, -1, 0), map_failed);
    EXPECT_EQ(take_errno(), guest(EBADF));

    for (const std::int64_t offset : {std::int64_t{0x1000}, -static_cast<std::int64_t>(page_size)}) {
        EXPECT_EQ(guest_mmap(nullptr, page_size, prot_read, map_private, fd_, offset), map_failed);
        EXPECT_EQ(take_errno(), guest(EINVAL));
    }

    // An anonymous mapping ignores its descriptor.
    void* const p = guest_mmap(nullptr, page_size, prot_read, map_private | map_anon, 999, 0);
    ASSERT_NE(p, map_failed);
    EXPECT_EQ(static_cast<const char*>(p)[0], 0);
    EXPECT_EQ(guest_munmap(p, page_size), 0);
}

}  // namespace
