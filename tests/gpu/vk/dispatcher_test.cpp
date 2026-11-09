// DISPATCH_DIRECT and DISPATCH_INDIRECT through the parser, the compute pipeline cache, the
// scheduler and the memory mirror on lavapipe (docs/design/graphics.md, item 25). The shader
// adds 1 to word 0 of the buffer that user data 0-3 describe (the shader uses no group ID), so
// the word counts the groups that ran, with a workgroup of one thread.
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "gpu/pm4/packet.hpp"
#include "gpu/pm4/parser.hpp"
#include "gpu/vk/compute_pipeline.hpp"
#include "gpu/vk/device.hpp"
#include "gpu/vk/dispatcher.hpp"
#include "gpu/vk/memory_mirror.hpp"
#include "gpu/vk/scheduler.hpp"
#include "hle/kernel/dmem.hpp"
#include "hle/kernel/vmem.hpp"
#include "test_device.hpp"
#include "gpu/gcn/encode.hpp"
#include "tools/mirror_observer.hpp"

namespace {

using namespace bb::hle::kernel;
using namespace bb::vk;
using bb::pm4::Parser;
using bb::pm4::RegisterMirror;
using bb::pm4::RunResult;
using bb::pm4::Stream;
using bb::test::gcn::mubuf;
using bb::test::gcn::sopp;
using bb::test::gcn::vop1;

constexpr std::uint64_t kib = 1024;
constexpr std::uint64_t code_at = 0, out_at = 0x1000, counts_at = 0x2000, bad_at = 0x3000;

void packet(std::vector<std::uint32_t>& out, std::uint8_t opcode,
            std::initializer_list<std::uint32_t> body) {
    out.push_back(bb::pm4::type3_header(opcode, static_cast<std::uint32_t>(body.size())));
    out.insert(out.end(), body);
}

class VkDispatcher : public testing::Test {
protected:
    void SetUp() override {
        const std::string selector = bb::test::vk_test_device();
        std::string why;
        instance_ = Instance::create(why);
        if (!instance_) {
            GTEST_SKIP() << "no Vulkan instance: " << why;
        }
        device_ = Device::create(*instance_, {.device = selector}, why);
        if (!device_) {
            GTEST_SKIP() << "no Vulkan device: " << why;
        }
        ASSERT_GE(dmem::fd(), 0);
        mirror_ = MemoryMirror::create(*device_,
                                       {.memfd = dmem::fd(),
                                        .window_start = vmem::window_start,
                                        .size = mirror_buffer_size,
                                        .min_block = 64 * kib},
                                       why);
        if (!mirror_) {
            GTEST_SKIP() << "no mirror: " << why;
        }
        observer_ = std::make_unique<bb::tools::MirrorObserver>(*mirror_);
        vmem::set_direct_observer(observer_.get());
        std::uint64_t offset = ~std::uint64_t{0};
        ASSERT_EQ(allocate_direct(0, direct_memory_size(), 256 * kib, 64 * kib, 0, &offset), 0);
        offset_ = offset;
        ASSERT_EQ(map_direct(&memory_, 256 * kib, 3, 0, offset, 64 * kib), 0);
        guest_ = reinterpret_cast<std::uintptr_t>(memory_);
        std::memset(memory_, 0, 256 * kib);

        // v_mov_b32 v1, 1; buffer_atomic_add v1, off, s[0:3], 0
        std::vector<std::uint32_t> code = {vop1(1, 1, 129)};
        for (const auto w : mubuf(50, 1, 0, 0, 128)) code.push_back(w);
        code.push_back(sopp(1));
        std::memcpy(bytes() + code_at, code.data(), code.size() * 4);
        std::memset(bytes() + bad_at, 0xFF, 16);

        scheduler_ = std::make_unique<Scheduler>(*device_);
        pipelines_ = std::make_unique<ComputePipelineCache>(
            *device_, [this](std::uint64_t address) -> std::span<const std::uint32_t> {
                if (address < guest_ || address >= guest_ + 256 * kib) return {};
                return {reinterpret_cast<const std::uint32_t*>(address),
                        (guest_ + 256 * kib - address) / 4};
            });
        dispatcher_ = std::make_unique<Dispatcher>(*device_, *scheduler_, *pipelines_, *mirror_);
        parser_ = std::make_unique<Parser>(regs_);
        parser_->set_dispatch([this](const RegisterMirror& r, const bb::pm4::DispatchCall& c) {
            dispatcher_->dispatch(r, c);
        });
        parser_->set_sync([this](bb::pm4::SyncPoint p) { dispatcher_->sync(p); });
    }

    void TearDown() override {
        dispatcher_.reset();  // before the memory it dispatches over goes
        if (memory_ != nullptr) {
            EXPECT_EQ(sceKernelMunmap(memory_, 256 * kib), 0);
            EXPECT_EQ(release_direct(offset_, 256 * kib), 0);
        }
        vmem::set_direct_observer(nullptr);
    }

    std::byte* bytes() const { return static_cast<std::byte*>(memory_); }
    std::uint32_t word(std::uint64_t at, unsigned i) const {
        std::uint32_t value;
        std::memcpy(&value, bytes() + at + 4 * i, 4);
        return value;
    }

    // The CS registers for the program at `code`: threads, program, RSRC2 (4 user SGPRs) and the user data (the output buffer's V#).
    void set_state(std::vector<std::uint32_t>& stream, std::uint64_t code) {
        const std::uint64_t out = guest_ + out_at;
        packet(stream, IT_SET_SH_REG, {0x207, 1, 1, 1});
        const std::uint64_t pgm = guest_ + code;
        packet(stream, IT_SET_SH_REG,
               {0x20C, static_cast<std::uint32_t>(pgm >> 8), static_cast<std::uint32_t>(pgm >> 40)});
        packet(stream, IT_SET_SH_REG, {0x213, 4u << 1});
        packet(stream, IT_SET_SH_REG,
               {0x240, static_cast<std::uint32_t>(out), static_cast<std::uint32_t>(out >> 32),
                0x1000, 0});
    }

    RunResult run(const std::vector<std::uint32_t>& stream) {
        Stream s(stream);
        const RunResult result = parser_->run(s);
        // Memory packets sync before a write; a stream's end needs the same to read the result.
        dispatcher_->sync(bb::pm4::SyncPoint::before_write);
        return result;
    }

    std::unique_ptr<Instance> instance_;
    std::unique_ptr<Device> device_;
    std::unique_ptr<MemoryMirror> mirror_;
    std::unique_ptr<bb::tools::MirrorObserver> observer_;
    std::unique_ptr<Scheduler> scheduler_;
    std::unique_ptr<ComputePipelineCache> pipelines_;
    std::unique_ptr<Dispatcher> dispatcher_;
    RegisterMirror regs_;
    std::unique_ptr<Parser> parser_;
    void* memory_ = nullptr;
    std::uint64_t offset_ = 0;
    std::uintptr_t guest_ = 0;
};

TEST_F(VkDispatcher, DirectDispatchRunsTheGroups) {
    std::vector<std::uint32_t> stream;
    set_state(stream, code_at);
    packet(stream, IT_DISPATCH_DIRECT, {2, 3, 2, 1});
    ASSERT_EQ(run(stream), RunResult::done);
    EXPECT_EQ(word(out_at, 0), 12u);
    EXPECT_EQ(word(out_at, 1), 0u);
    EXPECT_EQ(dispatcher_->stats().recorded, 1u);
    EXPECT_EQ(dispatcher_->stats().untranslated, 0u);
    EXPECT_EQ(dispatcher_->stats().failed, 0u);
}

// New code at the address of a cached program runs instead of the old one (#371).
TEST_F(VkDispatcher, CodeOverwrittenInPlaceRunsTheNewProgram) {
    std::vector<std::uint32_t> stream;
    set_state(stream, code_at);
    packet(stream, IT_DISPATCH_DIRECT, {2, 1, 1, 1});
    ASSERT_EQ(run(stream), RunResult::done);
    EXPECT_EQ(word(out_at, 0), 2u);

    const std::uint32_t add_two = vop1(1, 1, 130);  // v_mov_b32 v1, 2
    std::memcpy(bytes() + code_at, &add_two, 4);
    stream.clear();
    set_state(stream, code_at);
    packet(stream, IT_DISPATCH_DIRECT, {3, 1, 1, 1});
    ASSERT_EQ(run(stream), RunResult::done);
    EXPECT_EQ(word(out_at, 0), 2u + 3u * 2u);
    EXPECT_EQ(pipelines_->translations(), 2u);
}

TEST_F(VkDispatcher, IndirectDispatchReadsTheCountsAtBasePlusOffset) {
    const std::uint32_t counts[3] = {7, 1, 1};
    std::memcpy(bytes() + counts_at + 16, counts, sizeof counts);
    std::vector<std::uint32_t> stream;
    set_state(stream, code_at);
    const std::uint64_t base = guest_ + counts_at;
    packet(stream, IT_SET_BASE,
           {1, static_cast<std::uint32_t>(base), static_cast<std::uint32_t>(base >> 32)});
    packet(stream, IT_DISPATCH_INDIRECT, {16, 1});
    ASSERT_EQ(run(stream), RunResult::done);
    EXPECT_EQ(word(out_at, 0), 7u);
    EXPECT_EQ(word(out_at, 1), 0u);
    EXPECT_EQ(dispatcher_->stats().recorded, 1u);
}

TEST_F(VkDispatcher, SecondDispatchSeesTheFirstOnesWrites) {
    // Two dispatches in one recording with one pipeline.
    std::vector<std::uint32_t> stream;
    set_state(stream, code_at);
    packet(stream, IT_DISPATCH_DIRECT, {3, 1, 1, 1});
    packet(stream, IT_DISPATCH_DIRECT, {9, 1, 1, 1});
    ASSERT_EQ(run(stream), RunResult::done);
    EXPECT_EQ(word(out_at, 0), 12u);
    EXPECT_EQ(dispatcher_->stats().recorded, 2u);
    EXPECT_EQ(pipelines_->translations(), 1u);
}

TEST_F(VkDispatcher, DisabledComputeShaderDispatchesNothing) {
    std::vector<std::uint32_t> stream;
    set_state(stream, code_at);
    packet(stream, IT_DISPATCH_DIRECT, {5, 1, 1, 0});
    ASSERT_EQ(run(stream), RunResult::done);
    EXPECT_EQ(word(out_at, 0), 0u);
    EXPECT_EQ(dispatcher_->stats().recorded, 0u);
}

TEST_F(VkDispatcher, UntranslatedShadersAreCountedAndTheStreamGoesOn) {
    std::vector<std::uint32_t> stream;
    packet(stream, IT_DISPATCH_DIRECT, {1, 1, 1, 1});  // no program
    set_state(stream, bad_at);
    packet(stream, IT_DISPATCH_DIRECT, {1, 1, 1, 1});
    packet(stream, IT_DISPATCH_DIRECT, {1, 1, 1, 1});
    set_state(stream, code_at);
    packet(stream, IT_DISPATCH_DIRECT, {2, 1, 1, 1});
    ASSERT_EQ(run(stream), RunResult::done);
    EXPECT_EQ(dispatcher_->stats().untranslated, 3u);
    EXPECT_EQ(dispatcher_->stats().recorded, 1u);
    EXPECT_EQ(word(out_at, 0), 2u);
}

TEST_F(VkDispatcher, CountsOutsideTheMirrorFailTheDispatchOnly) {
    std::vector<std::uint32_t> stream;
    set_state(stream, code_at);
    const std::uint64_t base = vmem::window_start + 3 * (std::uint64_t{1} << 30);
    packet(stream, IT_SET_BASE,
           {1, static_cast<std::uint32_t>(base), static_cast<std::uint32_t>(base >> 32)});
    packet(stream, IT_DISPATCH_INDIRECT, {0, 1});
    packet(stream, IT_DISPATCH_DIRECT, {1, 1, 1, 1});
    ASSERT_EQ(run(stream), RunResult::done);
    EXPECT_EQ(dispatcher_->stats().failed, 1u);
    EXPECT_EQ(dispatcher_->stats().recorded, 1u);
    EXPECT_EQ(word(out_at, 0), 1u);
}

TEST_F(VkDispatcher, UnalignedCountsFailTheDispatchOnly) {
    std::vector<std::uint32_t> stream;
    set_state(stream, code_at);
    const std::uint64_t base = guest_ + counts_at;
    packet(stream, IT_SET_BASE,
           {1, static_cast<std::uint32_t>(base), static_cast<std::uint32_t>(base >> 32)});
    packet(stream, IT_DISPATCH_INDIRECT, {2, 1});
    packet(stream, IT_DISPATCH_DIRECT, {1, 1, 1, 1});
    ASSERT_EQ(run(stream), RunResult::done);
    EXPECT_EQ(dispatcher_->stats().failed, 1u);
    EXPECT_EQ(dispatcher_->stats().recorded, 1u);
    EXPECT_EQ(word(out_at, 0), 1u);
}

TEST_F(VkDispatcher, GroupCountsOverTheDeviceLimitFailTheDispatchOnly) {
    const std::uint32_t limit = device_->properties().core.limits.maxComputeWorkGroupCount[0];
    if (limit == UINT32_MAX) {
        GTEST_SKIP() << "the group count limit is the largest u32, so no count is over it";
    }
    const std::uint32_t over = limit + 1;
    std::vector<std::uint32_t> stream;
    set_state(stream, code_at);
    packet(stream, IT_DISPATCH_DIRECT, {over, 1, 1, 1});
    packet(stream, IT_DISPATCH_DIRECT, {1, 1, 1, 1});
    ASSERT_EQ(run(stream), RunResult::done);
    EXPECT_EQ(dispatcher_->stats().failed, 1u);
    EXPECT_EQ(dispatcher_->stats().recorded, 1u);
    EXPECT_EQ(word(out_at, 0), 1u);
}

TEST_F(VkDispatcher, ManyDispatchesRollOverTheDrawBlockChunks) {
    // 256 blocks per chunk: 600 dispatches use three chunks and defer the first two.
    constexpr unsigned count = 600;
    std::vector<std::uint32_t> stream;
    set_state(stream, code_at);
    for (unsigned i = 0; i < count; ++i) packet(stream, IT_DISPATCH_DIRECT, {1, 1, 1, 1});
    ASSERT_EQ(run(stream), RunResult::done);
    EXPECT_EQ(word(out_at, 0), count);
    EXPECT_EQ(dispatcher_->stats().recorded, count);
    EXPECT_EQ(dispatcher_->stats().failed, 0u);
}

TEST_F(VkDispatcher, SyncBeforeWaitSubmitsWithoutWaiting) {
    std::vector<std::uint32_t> stream;
    set_state(stream, code_at);
    packet(stream, IT_DISPATCH_DIRECT, {4, 1, 1, 1});
    Stream s(stream);
    ASSERT_EQ(parser_->run(s), RunResult::done);
    ASSERT_TRUE(scheduler_->recording());
    dispatcher_->sync(bb::pm4::SyncPoint::before_wait);
    EXPECT_FALSE(scheduler_->recording());
    EXPECT_GE(scheduler_->submitted(), 1u);
    dispatcher_->sync(bb::pm4::SyncPoint::before_write);
    EXPECT_EQ(word(out_at, 0), 4u);
}

}  // namespace
