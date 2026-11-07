// The graphics pipeline cache on lavapipe: pipelines for our own VS/PS pair and a fetch-calling
// VS, reused for an equal key, with the fetch address counting only for a program that calls it.

#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "gpu/gcn/encode.hpp"
#include "gpu/gcn/programs.hpp"
#include "gpu/vk/device.hpp"
#include "gpu/vk/pipeline_cache.hpp"

namespace {

using namespace bb::vk;
using bb::gcn::ShaderKey;
using bb::gcn::Stage;
using bb::test::gcn::sop1;
using bb::test::gcn::sopp;

constexpr std::uint64_t vs_address = 0x1000, ps_address = 0x2000;

class VkPipelineCache : public testing::Test {
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
        cache_ = std::make_unique<PipelineCache>(
            *device_, [this](std::uint64_t address) -> std::span<const std::uint32_t> {
                const auto it = code_.find(address);
                return it != code_.end() ? std::span(it->second) : std::span<std::uint32_t>();
            });
        // The render test's pair and keys (render_test.cpp) with the stages' push offsets.
        code_[vs_address] = bb::test::gcn::program_words("vs_attrs");
        code_[ps_address] = bb::test::gcn::program_words("ps_discard");
        pair_.vs = {vs_address, 0,
                    {.stage = Stage::vertex,
                     .pgm_rsrc1 = 3u << 24,
                     .pgm_rsrc2 = 4u << 1,
                     .instance_step_rate = {1, 2},
                     .push_offset = bb::gcn::vertex_push_offset}};
        pair_.ps = StageState{ps_address, 0,
                              {.stage = Stage::pixel,
                               .ps_input_ena = 1u << 1,
                               .ps_input_addr = 1u << 1,
                               .col_format = 9,
                               .quad_control = device_->quad_control(),
                               .push_offset = bb::gcn::pixel_push_offset}};
        color_.color[0] = VK_FORMAT_R32G32B32A32_SFLOAT;
        color_.color_count = 1;
    }

    std::unique_ptr<Instance> instance_;
    std::unique_ptr<Device> device_;
    std::map<std::uint64_t, std::vector<std::uint32_t>> code_;
    std::unique_ptr<PipelineCache> cache_;
    PipelineKey pair_;
    RenderingInfo color_;
    std::string why_;
};

TEST_F(VkPipelineCache, ReusesThePipelineOfAnEqualKey) {
    const GraphicsPipeline* first = cache_->get(pair_, color_, why_);
    ASSERT_NE(first, nullptr) << why_;
    EXPECT_NE(first->pipeline, VK_NULL_HANDLE);
    EXPECT_NE(first->layout, VK_NULL_HANDLE);

    PipelineKey equal = pair_;
    EXPECT_EQ(cache_->get(equal, color_, why_), first);
    // vs_attrs calls no fetch subroutine, so its fetch address doesn't count.
    equal.vs.fetch_address = 0x5000;
    EXPECT_EQ(cache_->get(equal, color_, why_), first);
    EXPECT_EQ(cache_->size(), 1u);
    EXPECT_EQ(cache_->translations(), 2u);

    // Another topology, target format or stage key is another pipeline.
    PipelineKey strip = pair_;
    strip.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    EXPECT_NE(cache_->get(strip, color_, why_), first);
    RenderingInfo unorm = color_;
    unorm.color[0] = VK_FORMAT_R8G8B8A8_UNORM;
    EXPECT_NE(cache_->get(pair_, unorm, why_), first);
    PipelineKey flipped = pair_;
    flipped.negative_one_to_one = flipped.depth_clamp = true;
    EXPECT_NE(cache_->get(flipped, color_, why_), first);
    EXPECT_EQ(cache_->size(), 4u) << why_;  // all made
    EXPECT_EQ(cache_->get(pair_, color_, why_), first);
}

// A depth-only pipeline whose VS calls the fetch subroutine (fetch_test.cpp): the fetch address
// is part of the key.
TEST_F(VkPipelineCache, ComparesTheFetchAddressOfAProgramThatCallsIt) {
    code_[0x3000] = {sop1(33, 4, 0), sopp(1)};  // s_swappc_b64 s[4:5], s[0:1]; s_endpgm
    code_[0x4000] = code_[0x4100] = {sop1(32, 0, 4)};  // s_setpc_b64 s[4:5]
    PipelineKey key;
    key.vs = {0x3000, 0x4000, {.stage = Stage::vertex, .pgm_rsrc2 = 4u << 1}};
    const RenderingInfo none;
    const GraphicsPipeline* first = cache_->get(key, none, why_);
    ASSERT_NE(first, nullptr) << why_;
    EXPECT_TRUE(first->vs.calls_fetch);
    key.vs.fetch_address = 0x4100;
    const GraphicsPipeline* second = cache_->get(key, none, why_);
    EXPECT_NE(second, nullptr) << why_;
    EXPECT_NE(second, first);
    key.vs.fetch_address = 0x4000;
    EXPECT_EQ(cache_->get(key, none, why_), first);
    EXPECT_EQ(cache_->translations(), 2u);
}

TEST_F(VkPipelineCache, KeepsTranslationErrorsButNotUnreadableCode) {
    PipelineKey bad = pair_;
    bad.vs.key.pgm_rsrc2 = 17u << 1;  // USER_SGPR 17
    EXPECT_EQ(cache_->get(bad, color_, why_), nullptr);
    const std::string first = why_;
    EXPECT_EQ(first, "the VS at 0x1000 doesn't translate: USER_SGPR 17 is more than 16 at offset "
                     "0x0 (word 0x00000000)");
    EXPECT_EQ(cache_->get(bad, color_, why_), nullptr);
    EXPECT_EQ(why_, first);
    EXPECT_EQ(cache_->translations(), 1u);
    EXPECT_EQ(cache_->size(), 1u);

    PipelineKey unmapped = pair_;
    unmapped.ps->code_address = 0x9000;
    EXPECT_EQ(cache_->get(unmapped, color_, why_), nullptr);
    EXPECT_EQ(why_, "no readable code at 0x9000");
    EXPECT_EQ(cache_->size(), 1u);
    code_[0x9000] = code_[ps_address];
    EXPECT_NE(cache_->get(unmapped, color_, why_), nullptr) << why_;
}

}  // namespace
