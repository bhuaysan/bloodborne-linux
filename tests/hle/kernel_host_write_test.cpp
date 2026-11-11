// System calls into tracked guest memory (docs/design/renderer.md, "System calls into tracked
// memory"): sceKernelRead and sceKernelPread announce the guest buffer before the host kernel
// writes it, so that the write tracking can lift its protection. Without that the system call
// fails with EFAULT on a write-protected page.

#include <fcntl.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "hle/kernel/abi.hpp"
#include "hle/kernel/file.hpp"
#include "hle/kernel/vmem.hpp"

namespace {

namespace fs = std::filesystem;
using namespace bb::hle::kernel;

std::int32_t sce(int host_errno) {
    return sce_error(guest_errno(host_errno).value());
}

// Stands in for the write tracking: records each announced range and makes `page` writable when
// the range starts in it.
class Unprotect final : public vmem::MappingObserver {
public:
    void mapped(std::uintptr_t, std::uint64_t, std::uint64_t) override {}
    void unmapped(std::uintptr_t, std::uint64_t, std::uint64_t) override {}
    void released(std::uint64_t, std::uint64_t) override {}
    void host_writing(std::uintptr_t address, std::uint64_t length) override {
        calls.emplace_back(address, length);
        const auto start = reinterpret_cast<std::uintptr_t>(page);
        if (address >= start && address - start < static_cast<std::uintptr_t>(::getpagesize())) {
            ::mprotect(page, ::getpagesize(), PROT_READ | PROT_WRITE);
        }
    }
    void* page = nullptr;
    std::vector<std::pair<std::uintptr_t, std::uint64_t>> calls;
};

// A read-only page, as the write tracking leaves a page the GPU caches hold, and a guest
// descriptor of a file that holds "tracked memory".
class HostWriteTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::string templ = (fs::temp_directory_path() / "bb-host-write-XXXXXX").string();
        ASSERT_NE(::mkdtemp(templ.data()), nullptr);
        tmp_ = templ;
        std::ofstream(tmp_ / "data") << text;
        const int host = ::open((tmp_ / "data").c_str(), O_RDONLY | O_CLOEXEC);
        ASSERT_GE(host, 0);
        fd_ = fd_table().insert(host);
        page_ = ::mmap(nullptr, ::getpagesize(), PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        ASSERT_NE(page_, MAP_FAILED);
        observer_.page = page_;
    }
    void TearDown() override {
        vmem::remove_mapping_observer(&observer_);
        ::munmap(page_, ::getpagesize());
        sceKernelClose(fd_);
        fs::remove_all(tmp_);
    }
    char* buffer() const {
        return static_cast<char*>(page_) + 0x10;
    }

    static constexpr std::string_view text = "tracked memory";
    fs::path tmp_;
    std::int32_t fd_ = -1;
    void* page_ = nullptr;
    Unprotect observer_;
};

TEST_F(HostWriteTest, ReadFailsWithEfaultWhenNobodyLiftsTheProtection) {
    const std::uint64_t before = vmem::host_writes();
    EXPECT_EQ(sceKernelRead(fd_, buffer(), text.size()), sce(EFAULT));
    EXPECT_EQ(sceKernelPread(fd_, buffer(), text.size(), 0), sce(EFAULT));
    EXPECT_EQ(vmem::host_writes(), before + 2);
}

TEST_F(HostWriteTest, ReadAnnouncesTheBufferFirst) {
    ASSERT_TRUE(vmem::add_mapping_observer(&observer_));
    const std::uint64_t before = vmem::host_writes();
    EXPECT_EQ(sceKernelRead(fd_, buffer(), text.size()), static_cast<std::int64_t>(text.size()));
    EXPECT_EQ(std::string_view(buffer(), text.size()), text);
    const std::vector<std::pair<std::uintptr_t, std::uint64_t>> expected{
        {reinterpret_cast<std::uintptr_t>(buffer()), text.size()}};
    EXPECT_EQ(observer_.calls, expected);
    EXPECT_EQ(vmem::host_writes(), before + 1);
}

TEST_F(HostWriteTest, PreadAnnouncesTheBufferFirst) {
    ASSERT_TRUE(vmem::add_mapping_observer(&observer_));
    EXPECT_EQ(sceKernelPread(fd_, buffer(), 6, 8), 6);
    EXPECT_EQ(std::string_view(buffer(), 6), "memory");
    const std::vector<std::pair<std::uintptr_t, std::uint64_t>> expected{
        {reinterpret_cast<std::uintptr_t>(buffer()), 6}};
    EXPECT_EQ(observer_.calls, expected);
}

// A bad descriptor fails before anything is announced.
TEST_F(HostWriteTest, ABadDescriptorAnnouncesNothing) {
    ASSERT_TRUE(vmem::add_mapping_observer(&observer_));
    EXPECT_EQ(sceKernelRead(-1, buffer(), 4), sce(EBADF));
    EXPECT_TRUE(observer_.calls.empty());
}

// Empty and null ranges are no writes; a range past the end of the address space is cut there.
TEST_F(HostWriteTest, EmptyRangesAreIgnoredAndLongOnesCut) {
    ASSERT_TRUE(vmem::add_mapping_observer(&observer_));
    const std::uint64_t before = vmem::host_writes();
    vmem::before_host_write(nullptr, 4);
    vmem::before_host_write(buffer(), 0);
    EXPECT_TRUE(observer_.calls.empty());
    EXPECT_EQ(vmem::host_writes(), before);
    const std::uintptr_t near_end = UINTPTR_MAX - 0xfff;
    vmem::before_host_write(reinterpret_cast<void*>(near_end), 0x2000);
    const std::vector<std::pair<std::uintptr_t, std::uint64_t>> expected{{near_end, 0xfff}};
    EXPECT_EQ(observer_.calls, expected);
    EXPECT_EQ(vmem::host_writes(), before + 1);
}

}  // namespace
