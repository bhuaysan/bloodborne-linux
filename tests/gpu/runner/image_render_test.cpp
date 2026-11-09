// The image sampling render tests on lavapipe (docs/design/graphics.md, "Image sampling"):
// vs_attrs draws one triangle over an 8x8 target and ps_sample samples a 16x16 image with five
// levels four ways, for a 2D image, a 2D array and a cube array. ps_load_mip reads levels of the
// same image with image_load_mip (docs/design/graphics.md, "Image loads").

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "gpu/gcn/programs.hpp"
#include "gpu/gcn/spirv_val.hpp"
#include "gpu/gcn/translate.hpp"
#include "gpu/runner/graphics_runner.hpp"

namespace {

using namespace bb::test::vk;
using bb::gcn::ImageClass;
using bb::gcn::ShaderKey;
using bb::gcn::Stage;

constexpr std::uint64_t base = 0x12'0000'0000;  // guest address of the target
constexpr std::uint32_t width = 8, height = 8, size = 16, levels = 5;
constexpr std::size_t vertex_offset = width * height * 16;  // the vertices follow the target
constexpr std::size_t texture_offset = vertex_offset + 256;

// vs_attrs's key, as in render_test.cpp.
const ShaderKey vertex_key = {.stage = Stage::vertex,
                              .pgm_rsrc1 = 3u << 24,
                              .pgm_rsrc2 = 4u << 1,
                              .instance_step_rate = {1, 2}};

// ps_sample's key: 12 user SGPRs, the perspective barycentrics at the center, mrt0 32_ABGR.
ShaderKey pixel_key(ImageClass c) {
    ShaderKey key{.stage = Stage::pixel, .pgm_rsrc2 = 12u << 1};
    key.ps_input_ena = key.ps_input_addr = 1u << 1;
    key.col_format = 9;
    key.images = {c};
    return key;
}

// Channel `c` of a texel: every level, layer and position gives a different value.
float texel(unsigned c, unsigned level, unsigned layer, unsigned x, unsigned y) {
    return static_cast<float>(c * 100000 + level * 10000 + layer * 1000 + y * 16 + x);
}

class ImageRender : public testing::Test {
protected:
    void SetUp() override {
        std::string why;
        runner_ = GraphicsRunner::create(why);
        if (!runner_) {
            GTEST_SKIP() << why;
        }
    }

    // Renders with an image of class `c` and `layers` layers. attr0 is (u, v, z, 2) with u and v
    // the pixel center over the target's size plus `shift` (a quarter texel of level 0 away from
    // the texel edges). The channels then read: x level 1 (implicit LOD: 16 texels over 8
    // pixels), y level 2 (LOD 2), z level 3 (derivatives of 0.5), w level 0, from the layers in
    // `layer`.
    void check(ImageClass c, std::uint32_t layers, float shift, float z,
               std::array<unsigned, 4> layer) {
        using bb::test::gcn::program_words;
        const auto vs = bb::gcn::translate(program_words("vs_attrs"), {}, vertex_key);
        const auto ps = bb::gcn::translate(program_words("ps_sample"), {}, pixel_key(c));
        if (bb::test::gcn::have_spirv_val()) {
            EXPECT_EQ(bb::test::gcn::validate(ps.spirv), "");
        }
        std::size_t texels = 0;
        for (unsigned level = 0; level < levels; ++level) {
            texels += (size >> level) * (size >> level) * layers;
        }
        auto memory = runner_->guest_memory(base, texture_offset + texels * 16);
        const float o = shift;
        const float vertices[3][8] = {{-1, -1, 0, 1, o, o, z, 2},
                                      {3, -1, 0, 1, 2 + o, o, z, 2},
                                      {-1, 3, 0, 1, o, 2 + o, z, 2}};
        std::memcpy(memory.data() + vertex_offset, vertices, sizeof vertices);
        auto* out = reinterpret_cast<float*>(memory.data() + texture_offset);
        for (unsigned level = 0; level < levels; ++level) {
            for (unsigned l = 0; l < layers; ++l) {
                for (unsigned y = 0; y < size >> level; ++y) {
                    for (unsigned x = 0; x < size >> level; ++x) {
                        for (unsigned ch = 0; ch < 4; ++ch) {
                            *out++ = texel(ch, level, l, x, y);
                        }
                    }
                }
            }
        }
        DrawBlock draw;  // s[0:3]: the V# of the vertices; the PS's T# and S# are not read
        const std::uint64_t at = base + vertex_offset;
        draw.user_data = {static_cast<std::uint32_t>(at),
                          static_cast<std::uint32_t>(at >> 32) | 32u << 16, 3, 0};
        const Texture texture{size, size, layers, levels, c.type != 9, texture_offset};
        runner_->render(vs.spirv, ps.spirv, draw, 3, {width, height, {}}, {&texture, 1}, 1);

        constexpr std::array<unsigned, 4> level = {1, 2, 3, 0};
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                SCOPED_TRACE(testing::Message() << "pixel " << x << ", " << y);
                std::array<float, 4> pixel;
                std::memcpy(pixel.data(), memory.data() + (y * width + x) * 16, 16);
                const double u = (x + 0.5) / width + 1.0 / 64, v = (y + 0.5) / height + 1.0 / 64;
                for (unsigned ch = 0; ch < 4; ++ch) {
                    const unsigned n = size >> level[ch];
                    EXPECT_EQ(pixel[ch], texel(ch, level[ch], layer[ch],
                                               static_cast<unsigned>(std::floor(u * n)),
                                               static_cast<unsigned>(std::floor(v * n))))
                        << "channel " << ch;
                }
            }
        }
    }

    void check_load(ImageClass c, std::uint32_t layers);

    std::unique_ptr<GraphicsRunner> runner_;
};

// ps_load_mip: attr0 interpolates to (x + 0.5, y + 0.5, 1.5, 3.5) at pixel (x, y) of the target, so
// red and green read level 1 at (x, y) and, with da, blue and alpha level 3 at slice 1 (a 2D image
// ignores the slice). Level 3 is 2x2: the rest of it reads 0 where the device has
// robustImageAccess2.
void ImageRender::check_load(ImageClass c, std::uint32_t layers) {
    using bb::test::gcn::program_words;
    const auto vs = bb::gcn::translate(program_words("vs_attrs"), {}, vertex_key);
    const auto ps = bb::gcn::translate(program_words("ps_load_mip"), {}, pixel_key(c));
    if (bb::test::gcn::have_spirv_val()) {
        EXPECT_EQ(bb::test::gcn::validate(ps.spirv), "");
    }
    std::size_t texels = 0;
    for (unsigned level = 0; level < levels; ++level) {
        texels += (size >> level) * (size >> level) * layers;
    }
    auto memory = runner_->guest_memory(base, texture_offset + texels * 16);
    const float vertices[3][8] = {{-1, -1, 0, 1, 0, 0, 1.5f, 3.5f},
                                  {3, -1, 0, 1, 16, 0, 1.5f, 3.5f},
                                  {-1, 3, 0, 1, 0, 16, 1.5f, 3.5f}};
    std::memcpy(memory.data() + vertex_offset, vertices, sizeof vertices);
    auto* out = reinterpret_cast<float*>(memory.data() + texture_offset);
    for (unsigned level = 0; level < levels; ++level) {
        for (unsigned l = 0; l < layers; ++l) {
            for (unsigned y = 0; y < size >> level; ++y) {
                for (unsigned x = 0; x < size >> level; ++x) {
                    for (unsigned ch = 0; ch < 4; ++ch) {
                        *out++ = texel(ch, level, l, x, y);
                    }
                }
            }
        }
    }
    DrawBlock draw;  // s[0:3]: the V# of the vertices; the PS's T# is not read
    const std::uint64_t at = base + vertex_offset;
    draw.user_data = {static_cast<std::uint32_t>(at),
                      static_cast<std::uint32_t>(at >> 32) | 32u << 16, 3, 0};
    const Texture texture{size, size, layers, levels, c.type != 9, texture_offset};
    runner_->render(vs.spirv, ps.spirv, draw, 3, {width, height, {}}, {&texture, 1}, 0);

    const unsigned slice = layers > 1 ? 1 : 0;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            SCOPED_TRACE(testing::Message() << "pixel " << x << ", " << y);
            std::array<float, 4> pixel;
            std::memcpy(pixel.data(), memory.data() + (y * width + x) * 16, 16);
            EXPECT_EQ(pixel[0], texel(0, 1, 0, x, y));
            EXPECT_EQ(pixel[1], texel(1, 1, 0, x, y));
            if (x < 2 && y < 2) {
                EXPECT_EQ(pixel[2], texel(2, 3, slice, x, y));
                EXPECT_EQ(pixel[3], texel(3, 3, slice, x, y));
            } else if (runner_->info().robust_image_access2) {
                EXPECT_EQ(pixel[2], 0.0f);
                EXPECT_EQ(pixel[3], 0.0f);
            }
        }
    }
}

TEST_F(ImageRender, LoadMipTwoD) { check_load({}, 1); }
TEST_F(ImageRender, LoadMipArray) { check_load({.type = 13}, 2); }

// The slice in z is ignored: the image has one layer.
TEST_F(ImageRender, TwoD) { check({}, 1, 1.0f / 64, 2, {0, 0, 0, 0}); }

// x and w have no da, so they read layer 0; y and z read the slice in z.
TEST_F(ImageRender, Array) { check({.type = 13}, 4, 1.0f / 64, 2, {0, 2, 2, 0}); }

// face_id 10 is face 2 of cube 1, layer 8 of the 2D array; x and y are in [1, 2].
TEST_F(ImageRender, CubeArray) { check({.type = 11}, 12, 1 + 1.0f / 64, 10, {8, 8, 8, 8}); }

}  // namespace
