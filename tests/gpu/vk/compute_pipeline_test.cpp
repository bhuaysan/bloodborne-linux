// The compute pipeline cache on lavapipe: a pipeline from synthetic CS registers, reused for an
// equal state, and another pipeline for another key.

#include <array>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "gpu/gcn/encode.hpp"
#include "gpu/vk/compute_pipeline.hpp"
#include "gpu/vk/device.hpp"

namespace {

using namespace bb::vk;
using bb::pm4::RegisterMirror;
using bb::pm4::RegSpace;
using bb::test::gcn::sopp;

constexpr std::uint64_t program = 0x12300;  // COMPUTE_PGM_LO is the address >> 8

// COMPUTE_NUM_THREAD_X..Z (0x2E07), COMPUTE_PGM_LO/HI (0x2E0C), _RSRC1/2 (0x2E12).
void set_cs(RegisterMirror& regs, std::uint64_t address, std::uint32_t rsrc2,
            std::uint32_t threads_x) {
    const std::array<std::uint32_t, 3> threads = {threads_x, 1, 1};
    regs.write(RegSpace::sh, 0x207, threads);
    const std::array<std::uint32_t, 2> pgm = {static_cast<std::uint32_t>(address >> 8),
                                              static_cast<std::uint32_t>(address >> 40)};
    regs.write(RegSpace::sh, 0x20C, pgm);
    const std::array<std::uint32_t, 2> rsrc = {0, rsrc2};
    regs.write(RegSpace::sh, 0x212, rsrc);
}

class VkComputePipeline : public testing::Test {
protected:
    void SetUp() override {
        ::setenv("VK_LOADER_DRIVERS_SELECT", "*lvp*", 0);  // as in device_test.cpp
        std::string why;
        instance_ = Instance::create(why);
        if (!instance_) {
            GTEST_SKIP() << "no Vulkan instance: " << why;
        }
        device_ = Device::create(*instance_, {.device = "llvmpipe"}, why);
        if (!device_) {
            GTEST_SKIP() << "no lavapipe device: " << why;
        }
        cache_ = std::make_unique<ComputePipelineCache>(
            *device_, [this](std::uint64_t address) -> std::span<const std::uint32_t> {
                const auto it = code_.find(address);
                return it != code_.end() ? std::span(it->second) : std::span<std::uint32_t>();
            });
        code_[program] = {sopp(1)};  // s_endpgm
    }

    std::unique_ptr<Instance> instance_;
    std::unique_ptr<Device> device_;
    std::map<std::uint64_t, std::vector<std::uint32_t>> code_;
    std::unique_ptr<ComputePipelineCache> cache_;
    RegisterMirror regs_;
    std::string why_;
};

// A dispatch can't start its group IDs elsewhere than 0 yet: the group ID would be wrong.
TEST_F(VkComputePipeline, RejectsADispatchStart) {
    set_cs(regs_, program, 2u << 1, 128);
    const std::array<std::uint32_t, 1> start = {4};
    regs_.write(RegSpace::sh, 0x205, start);  // COMPUTE_START_Y
    EXPECT_FALSE(compute_state(regs_, why_));
    EXPECT_EQ(why_, "COMPUTE_START_* 0x4 is not supported yet");
}

TEST_F(VkComputePipeline, StateComesFromTheCsRegisters) {
    EXPECT_FALSE(compute_state(regs_, why_));
    EXPECT_FALSE(why_.empty());
    set_cs(regs_, program, 2u << 1, 128);
    const auto state = compute_state(regs_, why_);
    ASSERT_TRUE(state) << why_;
    EXPECT_EQ(state->code_address, program);
    EXPECT_EQ(state->key.stage, bb::gcn::Stage::compute);
    EXPECT_EQ(state->key.pgm_rsrc2, 2u << 1);
    EXPECT_EQ(state->key.num_threads, (std::array<std::uint32_t, 3>{128, 1, 1}));
    EXPECT_EQ(state->key.push_offset, 0u);
}

TEST_F(VkComputePipeline, ReusesThePipelineOfAnEqualState) {
    set_cs(regs_, program, 0, 64);
    const auto state = compute_state(regs_, why_);
    ASSERT_TRUE(state) << why_;
    const ComputePipeline* first = cache_->get(*state, why_);
    ASSERT_NE(first, nullptr) << why_;
    EXPECT_NE(first->pipeline, VK_NULL_HANDLE);
    EXPECT_NE(first->layout, VK_NULL_HANDLE);

    // The same registers again, and user data (not part of the key) written in between.
    const std::array<std::uint32_t, 2> user = {7, 8};
    regs_.write(RegSpace::sh, 0x240, user);
    EXPECT_EQ(cache_->get(*compute_state(regs_, why_), why_), first);
    EXPECT_EQ(cache_->size(), 1u);
    EXPECT_EQ(cache_->translations(), 1u);

    // Another workgroup size is another pipeline.
    set_cs(regs_, program, 0, 32);
    const ComputePipeline* other = cache_->get(*compute_state(regs_, why_), why_);
    ASSERT_NE(other, nullptr) << why_;
    EXPECT_NE(other, first);
    EXPECT_EQ(cache_->size(), 2u);
    EXPECT_EQ(cache_->get(*state, why_), first);
}

TEST_F(VkComputePipeline, KeepsTranslationErrorsButNotUnreadableCode) {
    ComputeState state;
    state.code_address = 0x99900;
    EXPECT_EQ(cache_->get(state, why_), nullptr);
    EXPECT_NE(why_.find("no readable code"), std::string::npos);
    EXPECT_EQ(cache_->size(), 0u);

    // SCRATCH_EN (RSRC2 bit 0) is rejected by the translator.
    state.code_address = program;
    state.key.pgm_rsrc2 = 1;
    EXPECT_EQ(cache_->get(state, why_), nullptr);
    EXPECT_NE(why_.find("doesn't translate"), std::string::npos);
    std::string again;
    EXPECT_EQ(cache_->get(state, again), nullptr);
    EXPECT_EQ(again, why_);
    EXPECT_EQ(cache_->size(), 1u);
    EXPECT_EQ(cache_->translations(), 1u);
}

TEST_F(VkComputePipeline, ADeviceFailureIsNotKept) {
    set_cs(regs_, program, 0, 64);
    const auto state = compute_state(regs_, why_);
    ASSERT_TRUE(state) << why_;
    bool fail = true;
    cache_->set_create_hook([&] {
        if (fail) throw std::runtime_error("injected");
    });
    EXPECT_THROW(cache_->get(*state, why_), std::runtime_error);
    EXPECT_EQ(cache_->size(), 0u);
    // The next request tries again instead of answering with a kept error.
    fail = false;
    const ComputePipeline* pipeline = cache_->get(*state, why_);
    ASSERT_NE(pipeline, nullptr) << why_;
    EXPECT_NE(pipeline->pipeline, VK_NULL_HANDLE);
    EXPECT_EQ(cache_->size(), 1u);
    EXPECT_EQ(cache_->translations(), 2u);
}

}  // namespace
