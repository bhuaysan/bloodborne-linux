// The graphics pipeline cache on lavapipe: pipelines for our own VS/PS pair and a fetch-calling
// VS, reused for an equal key, with the fetch address counting only for a program that calls it.

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

// The raster and blend state of the key (#281, part 2): another polygon mode or blend state is
// another pipeline, and the state of a target that isn't bound is not part of the key.
TEST_F(VkPipelineCache, ComparesThePolygonModeAndTheBlendState) {
    const GraphicsPipeline* first = cache_->get(pair_, color_, why_);
    ASSERT_NE(first, nullptr) << why_;
    PipelineKey wire = pair_;
    wire.polygon_mode = VK_POLYGON_MODE_LINE;
    EXPECT_NE(cache_->get(wire, color_, why_), first);
    PipelineKey blend = pair_;
    blend.blend[0] = {.enable = true,
                      .write_mask = 0x7,
                      .src_color = VK_BLEND_FACTOR_SRC_ALPHA,
                      .dst_color = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                      .color_op = VK_BLEND_OP_MAX};
    EXPECT_NE(cache_->get(blend, color_, why_), first) << why_;
    EXPECT_EQ(cache_->size(), 3u);
    PipelineKey unbound = pair_;
    unbound.blend[3].write_mask = 0;  // color_ binds target 0 only
    EXPECT_EQ(cache_->get(unbound, color_, why_), first);
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

// The cache hashes the code of both stages and the fetch subroutine on every request (#371).
TEST_F(VkPipelineCache, CodeChangedAtTheSameAddressIsAnotherPipeline) {
    const GraphicsPipeline* first = cache_->get(pair_, color_, why_);
    ASSERT_NE(first, nullptr) << why_;

    // Another pixel shader.
    const auto ps = code_[ps_address];
    code_[ps_address] = std::vector<std::uint32_t>{sopp(0), sopp(1)};  // s_nop; s_endpgm
    const GraphicsPipeline* second = cache_->get(pair_, color_, why_);
    ASSERT_NE(second, nullptr) << why_;
    EXPECT_NE(second, first);
    EXPECT_EQ(cache_->get(pair_, color_, why_), second);
    code_[ps_address] = ps;
    EXPECT_EQ(cache_->get(pair_, color_, why_), first);

    // Another vertex shader.
    const auto vs = code_[vs_address];
    code_[vs_address] = std::vector<std::uint32_t>{sopp(0), sopp(1)};
    const GraphicsPipeline* third = cache_->get(pair_, color_, why_);
    ASSERT_NE(third, nullptr) << why_;
    EXPECT_NE(third, first);
    EXPECT_NE(third, second);
    code_[vs_address] = vs;
    EXPECT_EQ(cache_->get(pair_, color_, why_), first);
    EXPECT_EQ(cache_->translations(), 6u);
    EXPECT_EQ(cache_->size(), 3u);
}

TEST_F(VkPipelineCache, ChangedFetchSubroutineAndNewFetchCallsAreNoticed) {
    code_[0x3000] = {sop1(33, 4, 0), sopp(1)};  // s_swappc_b64 s[4:5], s[0:1]; s_endpgm
    code_[0x4000] = {sop1(32, 0, 4)};           // s_setpc_b64 s[4:5]
    PipelineKey key;
    key.vs = {0x3000, 0x4000, {.stage = Stage::vertex, .pgm_rsrc2 = 4u << 1}};
    const RenderingInfo none;
    const GraphicsPipeline* first = cache_->get(key, none, why_);
    ASSERT_NE(first, nullptr) << why_;
    code_[0x4000] = {sopp(0), sop1(32, 0, 4)};  // an s_nop first
    const GraphicsPipeline* second = cache_->get(key, none, why_);
    ASSERT_NE(second, nullptr) << why_;
    EXPECT_NE(second, first);

    // A program that didn't call the fetch subroutine does now: the fetch address counts again.
    code_[0x5000] = {sopp(1)};
    key.vs = {0x5000, 0x4000, {.stage = Stage::vertex, .pgm_rsrc2 = 4u << 1}};
    const GraphicsPipeline* plain = cache_->get(key, none, why_);
    ASSERT_NE(plain, nullptr) << why_;
    EXPECT_FALSE(plain->vs.calls_fetch);
    code_[0x5000] = code_[0x3000];
    const GraphicsPipeline* calling = cache_->get(key, none, why_);
    ASSERT_NE(calling, nullptr) << why_;
    EXPECT_TRUE(calling->vs.calls_fetch);
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

// A rejected program is stamped with the dwords that decide the error, not the mapping after it.
TEST_F(VkPipelineCache, KeptErrorIgnoresDataAfterTheRejectedInstruction) {
    code_[vs_address] = {0xffffffffu, 0, 0, 0, 0, 0};  // no instruction
    EXPECT_EQ(cache_->get(pair_, color_, why_), nullptr);
    const std::string first = why_;
    EXPECT_NE(first.find("doesn't translate"), std::string::npos);
    code_[vs_address][5] = 0x1234;  // a neighbour's data
    EXPECT_EQ(cache_->get(pair_, color_, why_), nullptr);
    EXPECT_EQ(why_, first);
    EXPECT_EQ(cache_->translations(), 1u);
    EXPECT_EQ(cache_->size(), 1u);
    code_[vs_address][0] = sopp(1);  // the instruction itself changes: translated again
    EXPECT_NE(cache_->get(pair_, color_, why_), nullptr) << why_;
    EXPECT_EQ(cache_->translations(), 3u);  // the VS and the PS
}

TEST_F(VkPipelineCache, ADeviceFailureIsNotKept) {
    bool fail = true;
    cache_->set_create_hook([&] {
        if (fail) throw std::runtime_error("injected");
    });
    EXPECT_THROW(cache_->get(pair_, color_, why_), std::runtime_error);
    EXPECT_EQ(cache_->size(), 0u);
    fail = false;
    ASSERT_NE(cache_->get(pair_, color_, why_), nullptr) << why_;
    EXPECT_EQ(cache_->size(), 1u);
}

TEST_F(VkPipelineCache, AnUnmappedFetchSubroutineIsNotKept) {
    code_[0x3000] = {sop1(33, 4, 0), sopp(1)};
    PipelineKey key;
    key.vs = {0x3000, 0x4000, {.stage = Stage::vertex, .pgm_rsrc2 = 4u << 1}};
    const RenderingInfo none;
    EXPECT_EQ(cache_->get(key, none, why_), nullptr);
    EXPECT_EQ(why_, "no readable code at 0x4000");
    EXPECT_EQ(cache_->size(), 0u);
    code_[0x4000] = {sop1(32, 0, 4)};
    EXPECT_NE(cache_->get(key, none, why_), nullptr) << why_;
}

TEST_F(VkPipelineCache, AVsErrorIndependentOfTheFetchIsKeptOncePerProgram) {
    PipelineKey bad = pair_;
    bad.vs.key.pgm_rsrc2 = 17u << 1;
    for (std::uint64_t pointer = 0x5000; pointer < 0x5004; ++pointer) {
        bad.vs.fetch_address = pointer;  // a per-draw pointer in the user data
        EXPECT_EQ(cache_->get(bad, color_, why_), nullptr);
    }
    EXPECT_EQ(cache_->size(), 1u);
    EXPECT_EQ(cache_->translations(), 1u);
}

}  // namespace
