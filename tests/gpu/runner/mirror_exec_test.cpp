// bbvk's GPU memory mirror on lavapipe (docs/design/graphics.md, "GPU memory mirror"): a 2 GiB
// mirror with RADV's 64 KiB blocks over a window of the test's own, memfd views bound and
// unbound as the kernel maps and unmaps them, read and written by the spike's mem_mirror program
// through the mirror's bias table.

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "gpu/gcn/programs.hpp"
#include "gpu/gcn/translate.hpp"
#include "gpu/runner/compute_runner.hpp"
#include "gpu/vk/memory_mirror.hpp"

namespace {

using bb::vk::MemoryMirror;
using bb::vk::mirror_buffer_size;

constexpr std::uint64_t kib = 1024, page = 16 * kib, block = 64 * kib;

// The runner, with its device for the mirror.
struct Runner : bb::test::vk::ComputeRunner {
    explicit Runner(ComputeRunner&& base) : ComputeRunner(std::move(base)) {}
    using ComputeRunner::device;
};

class MirrorExec : public testing::Test {
protected:
    void SetUp() override {
        std::string why;
        auto base = bb::test::vk::ComputeRunner::create(why);
        if (!base) {
            GTEST_SKIP() << why;
        }
        runner_.emplace(std::move(*base));
        // A window of 2 GiB, aligned to 2 GiB, and a memfd of 1 MiB sealed against shrinking.
        reserved_ = ::mmap(nullptr, 2 * mirror_buffer_size, PROT_NONE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        memfd_ = ::memfd_create("mirror-exec-test", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        ASSERT_NE(reserved_, MAP_FAILED);
        ASSERT_GE(memfd_, 0);
        ASSERT_EQ(::ftruncate(memfd_, 1024 * kib), 0);
        ASSERT_EQ(::fcntl(memfd_, F_ADD_SEALS, F_SEAL_SHRINK), 0);
        start_ = (reinterpret_cast<std::uintptr_t>(reserved_) + mirror_buffer_size - 1) &
                 ~(mirror_buffer_size - 1);
        mirror_ = MemoryMirror::create(runner_->device(),
                                       {.memfd = memfd_, .window_start = start_,
                                        .size = mirror_buffer_size, .min_block = block},
                                       why);
        if (!mirror_) {
            GTEST_SKIP() << why;
        }
        ASSERT_EQ(mirror_->block(), block);
        spirv_ = bb::gcn::translate(bb::test::gcn::program_words("mem_mirror"), {},
                                    {.pgm_rsrc2 = bb::test::gcn::program_pgm_rsrc2("mem_mirror"),
                                     .num_threads = {64, 1, 1}})
                     .spirv;
    }

    void TearDown() override {
        mirror_.reset();  // its imports pin pages of the memfd, not the views
        if (reserved_ != MAP_FAILED) {
            ::munmap(reserved_, 2 * mirror_buffer_size);
        }
        if (memfd_ >= 0) {
            ::close(memfd_);
        }
    }

    // Maps and binds the view of the memfd at `offset` at window offset `window`, as the kernel
    // will.
    std::uintptr_t map(std::uint64_t window, std::uint64_t length, std::uint64_t offset) {
        void* p = ::mmap(reinterpret_cast<void*>(start_ + window), length,
                         PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, memfd_,
                         static_cast<off_t>(offset));
        EXPECT_NE(p, MAP_FAILED);
        if (p == MAP_FAILED) {
            return 0;
        }
        EXPECT_TRUE(mirror_->bind(start_ + window, length, offset));
        return start_ + window;
    }

    void unmap(std::uintptr_t address, std::uint64_t length, std::uint64_t offset) {
        mirror_->unbind(address, length, offset);
        EXPECT_NE(::mmap(reinterpret_cast<void*>(address), length, PROT_NONE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0),
                  MAP_FAILED);
    }

    // Whether the program sees `value` that the CPU wrote at `address` and writes value + i for
    // lane i at address + 0x100, which the CPU then reads.
    void expect_round_trip(std::uintptr_t address, std::uint32_t value) {
        ASSERT_NE(address, 0u) << "the mapping failed";
        auto* p = reinterpret_cast<std::uint32_t*>(address);
        p[0] = value;
        const std::uintptr_t out = address + 0x100;
        bb::test::vk::DrawBlock draw;
        draw.user_data = {static_cast<std::uint32_t>(out), static_cast<std::uint32_t>(out >> 32),
                          static_cast<std::uint32_t>(address),
                          static_cast<std::uint32_t>(address >> 32)};
        draw.bias_table = mirror_->bias_table();
        runner_->run(spirv_, draw, {1, 1, 1});
        for (std::uint32_t i = 0; i < 64; ++i) {
            ASSERT_EQ(p[0x40 + i], value + i) << "lane " << i << " at " << std::hex << address;
        }
    }

    std::optional<Runner> runner_;
    void* reserved_ = MAP_FAILED;
    int memfd_ = -1;
    std::uintptr_t start_ = 0;
    std::unique_ptr<MemoryMirror> mirror_;
    std::vector<std::uint32_t> spirv_;
};

// A guest page in the middle of a block, unmapped and replaced by another view of the memfd at
// the same address: the GPU sees the new view, and nothing stays imported.
TEST_F(MirrorExec, MapAndUnmapKeepTheMirrorConsistent) {
    const std::uintptr_t a = map(3 * block + page, page, page);
    expect_round_trip(a, 0x1234'0000);
    expect_round_trip(a, 0xabcd'0000);  // what the CPU wrote since
    EXPECT_EQ(mirror_->imports(), 1u);
    unmap(a, page, page);
    EXPECT_EQ(mirror_->imports(), 0u);
    const std::uintptr_t b = map(3 * block + page, page, 5 * block + page);
    expect_round_trip(b, 0x5678'0000);
    unmap(b, page, 5 * block + page);
    EXPECT_EQ(mirror_->imports(), 0u);
}

// Two views of contiguous memfd pages in one block share its binding, which stays until both
// are unmapped. A mapping in the mirror's last block binds too.
TEST_F(MirrorExec, BlocksCountTheirPages) {
    const std::uintptr_t a = map(7 * block, page, 2 * block);
    const std::uintptr_t b = map(7 * block + page, 2 * page, 2 * block + page);
    EXPECT_EQ(mirror_->imports(), 1u);
    unmap(a, page, 2 * block);
    expect_round_trip(b + page, 0x2222'0000);
    unmap(b, 2 * page, 2 * block + page);
    EXPECT_EQ(mirror_->imports(), 0u);
    const std::uintptr_t c = map(mirror_buffer_size - block, block, 0);
    expect_round_trip(c, 0x3333'0000);
    unmap(c, block, 0);
}

// Unbinding the middle of a bound run unbinds only its blocks and keeps the import for the
// rest. The mirror reports no per-block state, so the middle block's unbinding shows in the
// import count: binding it again needs a new import (2), where a block still bound would not.
TEST_F(MirrorExec, PartialUnbindKeepsTheRest) {
    const std::uintptr_t a = map(16 * block, 3 * block, 4 * block);
    EXPECT_EQ(mirror_->imports(), 1u);
    mirror_->unbind(a + block, block, 5 * block);
    EXPECT_EQ(mirror_->imports(), 1u);
    expect_round_trip(a, 0x4444'0000);
    expect_round_trip(a + 2 * block, 0x5555'0000);
    EXPECT_TRUE(mirror_->bind(a + block, block, 5 * block));
    EXPECT_EQ(mirror_->imports(), 2u);
    expect_round_trip(a + block, 0x6666'0000);
    unmap(a, block, 4 * block);
    unmap(a + block, block, 5 * block);
    unmap(a + 2 * block, block, 6 * block);
    EXPECT_EQ(mirror_->imports(), 0u);
}

// Ranges that break the congruence, reach past the mirror or aren't whole pages bind nothing.
TEST_F(MirrorExec, RefusesWhatItCantMirror) {
    EXPECT_FALSE(mirror_->bind(start_ + page, page, 0));
    EXPECT_FALSE(mirror_->bind(start_ + mirror_buffer_size - page, 2 * page, block - page));
    EXPECT_FALSE(mirror_->bind(start_ - page, page, block - page));
    EXPECT_FALSE(mirror_->bind(start_, page + 1, 0));
    EXPECT_EQ(mirror_->imports(), 0u);
}

}  // namespace
