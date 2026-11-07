// The render test of a translated VS/PS pair on lavapipe (docs/design/graphics.md, "Graphics
// stages"): vs_attrs draws one triangle over the whole target, and ps_discard, in whole quad mode
// for its interpolation, discards the left half and writes its interpolated attribute elsewhere.

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include "gpu/gcn/programs.hpp"
#include "gpu/gcn/spirv_val.hpp"
#include "gpu/gcn/translate.hpp"
#include "gpu/runner/graphics_runner.hpp"

namespace {

using namespace bb::test::vk;
using bb::gcn::ShaderKey;
using bb::gcn::Stage;

constexpr std::uint64_t base = 0x12'0000'0000;  // guest address of the image
constexpr std::uint32_t width = 16, height = 8;
constexpr std::size_t vertex_offset = width * height * 16;  // the vertices follow the image
constexpr std::array<float, 4> clear = {-1, -1, -1, -1};

// vs_attrs's key, as in stages_test.cpp: 4 user SGPRs, VGPR_COMP_CNT 3, step rates 1 and 2.
const ShaderKey vertex_key = {.stage = Stage::vertex,
                              .pgm_rsrc1 = 3u << 24,
                              .pgm_rsrc2 = 4u << 1,
                              .instance_step_rate = {1, 2}};

// ps_discard's key: no user SGPRs, the perspective barycentrics at the center (PERSP_CENTER, bit
// 1), attr0 at location 0, mrt0 32_ABGR (9), and the device flag quad_control.
ShaderKey pixel_key(bool quad_control) {
    ShaderKey key{.stage = Stage::pixel};
    key.ps_input_ena = key.ps_input_addr = 1u << 1;
    key.col_format = 9;
    key.quad_control = quad_control;
    return key;
}

class Render : public testing::Test {
protected:
    void SetUp() override {
        std::string why;
        runner_ = GraphicsRunner::create(why);
        if (!runner_) {
            GTEST_SKIP() << why;
        }
    }

    // Renders the pair and checks every pixel: the triangle (-1,-1), (3,-1), (-1,3) covers the
    // target, and its attribute (x, y, 0.25, 1) is 0 at the first vertex and 2 in x and y at the
    // others, so at a pixel center it is the center over the target's size. The pixels whose x
    // is below 0.5 are discarded and keep the clear color. `quad_control` is the pixel key's
    // device flag.
    void check(bool quad_control = false) {
        using bb::test::gcn::program_words;
        const auto vs = bb::gcn::translate(program_words("vs_attrs"), {}, vertex_key);
        const auto ps =
            bb::gcn::translate(program_words("ps_discard"), {}, pixel_key(quad_control));
        if (bb::test::gcn::have_spirv_val()) {
            EXPECT_EQ(bb::test::gcn::validate(vs.spirv), "");
            EXPECT_EQ(bb::test::gcn::validate(ps.spirv), "");
        }
        const float vertices[3][8] = {{-1, -1, 0, 1, 0, 0, 0.25f, 1},
                                      {3, -1, 0, 1, 2, 0, 0.25f, 1},
                                      {-1, 3, 0, 1, 0, 2, 0.25f, 1}};
        auto memory = runner_->guest_memory(base, vertex_offset + sizeof vertices);
        std::memcpy(memory.data() + vertex_offset, vertices, sizeof vertices);
        DrawBlock draw;  // s[0:3]: the V# of the vertices, 3 records of 32 bytes
        const std::uint64_t at = base + vertex_offset;
        draw.user_data = {static_cast<std::uint32_t>(at),
                          static_cast<std::uint32_t>(at >> 32) | 32u << 16, 3, 0};
        runner_->render(vs.spirv, ps.spirv, draw, 3, {width, height, clear});

        int kept = 0;
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                SCOPED_TRACE(testing::Message() << "pixel " << x << ", " << y);
                std::array<float, 4> pixel;
                std::memcpy(pixel.data(), memory.data() + (y * width + x) * 16, 16);
                const float u = (x + 0.5f) / width, v = (y + 0.5f) / height;
                if (u < 0.5f) {
                    EXPECT_EQ(pixel, clear);
                    continue;
                }
                ++kept;
                const std::array<float, 4> expected = {u, v, 0.25f, 1};
                for (int c = 0; c < 4; ++c) {
                    EXPECT_NEAR(pixel[c], expected[c], 1e-5f) << "channel " << c;
                }
            }
        }
        EXPECT_EQ(kept, static_cast<int>(width * height / 2));
    }

    std::unique_ptr<GraphicsRunner> runner_;
};

TEST_F(Render, VertexPixelWithDiscard) { check(); }

// The same pair with QuadDerivativesKHR and RequireFullQuadsKHR, on a device with
// shaderQuadControl (docs/design/graphics.md, "Quad control").
TEST_F(Render, QuadControl) {
    if (!runner_->info().quad_control) {
        GTEST_SKIP() << "no shaderQuadControl on " << runner_->info().driver;
    }
    check(true);
}

TEST_F(Render, TargetMustFitGuestMemory) {
    EXPECT_THROW(runner_->render({}, {}, {}, 3, {width, height, clear}), std::logic_error);
    runner_->guest_memory(base, width * height * 16 - 1);
    EXPECT_THROW(runner_->render({}, {}, {}, 3, {width, height, clear}), std::logic_error);
}

}  // namespace
