// The image sampling render tests on lavapipe (docs/design/graphics.md, "Image sampling"):
// vs_attrs draws one triangle over an 8x8 target and ps_sample samples a 16x16 image with five
// levels four ways, for a 2D image, a 2D array and a cube array. ps_load_mip reads levels of the
// same image with image_load_mip, and mem_image_store writes a storage image in the compute stage
// (docs/design/graphics.md, "Image loads and stores").

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

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

    void check_load(ImageClass c, std::uint32_t layers, unsigned z, unsigned w);

    std::unique_ptr<GraphicsRunner> runner_;
};

// ps_load_mip: attr0 interpolates to (x + 0.5, y + 0.5, z + 0.5, w + 0.5) at pixel (x, y) of the
// target, so red and green read level z at (x, y) of layer 0 and, with da, blue and alpha level w
// at slice z (a 2D image ignores the slice). Whatever is out of range (texel, level or slice) reads
// 0, which robustImageAccess2 guarantees; the runner's lavapipe must have it, or this check of the
// out-of-range reads would mean nothing.
void ImageRender::check_load(ImageClass c, std::uint32_t layers, unsigned z, unsigned w) {
    ASSERT_TRUE(runner_->info().robust_image_access2);
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
    const float zf = z + 0.5f, wf = w + 0.5f;
    const float vertices[3][8] = {{-1, -1, 0, 1, 0, 0, zf, wf},
                                  {3, -1, 0, 1, 16, 0, zf, wf},
                                  {-1, 3, 0, 1, 0, 16, zf, wf}};
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

    const unsigned slice = c.type == 13 ? z : 0;
    const auto loaded = [&](unsigned ch, unsigned level, unsigned layer, unsigned x, unsigned y) {
        const bool inside = level < levels && layer < layers && x < size >> level &&
                            y < size >> level;
        return inside ? texel(ch, level, layer, x, y) : 0.0f;
    };
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            SCOPED_TRACE(testing::Message() << "pixel " << x << ", " << y);
            std::array<float, 4> pixel;
            std::memcpy(pixel.data(), memory.data() + (y * width + x) * 16, 16);
            EXPECT_EQ(pixel[0], loaded(0, z, 0, x, y));
            EXPECT_EQ(pixel[1], loaded(1, z, 0, x, y));
            EXPECT_EQ(pixel[2], loaded(2, w, slice, x, y));
            EXPECT_EQ(pixel[3], loaded(3, w, slice, x, y));
        }
    }
}

// Level 3 is 2x2, so most of the blue and alpha reads are out of range.
TEST_F(ImageRender, LoadMipTwoD) { check_load({}, 1, 1, 3); }
TEST_F(ImageRender, LoadMipArray) { check_load({.type = 13}, 2, 1, 3); }
// Level 5 is the first one past the image's five.
TEST_F(ImageRender, LoadMipLevelOutOfRange) { check_load({}, 1, 5, 5); }
TEST_F(ImageRender, LoadMipArrayLevelOutOfRange) { check_load({.type = 13}, 2, 1, 5); }
// Slice 2 is the first one past the array's two; red and green then read level 2.
TEST_F(ImageRender, LoadMipSliceOutOfRange) { check_load({.type = 13}, 2, 2, 3); }

// The slice in z is ignored: the image has one layer.
TEST_F(ImageRender, TwoD) { check({}, 1, 1.0f / 64, 2, {0, 0, 0, 0}); }

// x and w have no da, so they read layer 0; y and z read the slice in z.
TEST_F(ImageRender, Array) { check({.type = 13}, 4, 1.0f / 64, 2, {0, 2, 2, 0}); }

// face_id 10 is face 2 of cube 1, layer 8 of the 2D array; x and y are in [1, 2].
TEST_F(ImageRender, CubeArray) { check({.type = 11}, 12, 1 + 1.0f / 64, 10, {8, 8, 8, 8}); }

// mem_image_store runs 8 lanes on a 4x4 image, with lane 3 inactive. Each lane stores to (x, 1)
// with dmask 0x9 and, with da, to (x, 2) of slice 1 with dmask 0x7. Lanes 4 to 7 are out of range,
// so robust image access drops those stores.
void check_store(GraphicsRunner& runner, ImageClass c, std::uint32_t layers,
                 VkFormat format = VK_FORMAT_R32G32B32A32_SFLOAT) {
    constexpr std::uint32_t side = 4, untouched = 0xffffffff;
    ShaderKey key{.pgm_rsrc2 = 13u << 1, .num_threads = {8, 1, 1}};
    key.images = {c};
    const auto t = bb::gcn::translate(bb::test::gcn::program_words("mem_image_store"), {}, key);
    EXPECT_EQ(t.interface.stored_images, std::vector<bool>{true});
    if (bb::test::gcn::have_spirv_val()) {
        EXPECT_EQ(bb::test::gcn::validate(t.spirv), "");
    }
    auto memory = runner.guest_memory(base, std::size_t{side} * side * layers * 16);
    std::memset(memory.data(), 0xff, memory.size());

    runner.dispatch(t.spirv, {}, {1, 1, 1},
                    {.width = side, .height = side, .layers = layers, .arrayed = c.type != 9,
                     .format = format});

    // The bits of a float: the store writes what is in the VGPRs, for every number format.
    const auto bits = [](float value) { return std::bit_cast<std::uint32_t>(value); };
    const std::uint32_t slice = layers > 1 ? 1 : 0;
    for (std::uint32_t l = 0; l < layers; ++l) {
        for (std::uint32_t y = 0; y < side; ++y) {
            for (std::uint32_t x = 0; x < side; ++x) {
                SCOPED_TRACE(testing::Message() << "layer " << l << " row " << y << " x " << x);
                std::array<std::uint32_t, 4> stored = {untouched, untouched, untouched, untouched};
                if (x != 3) {  // lane 3 is inactive
                    const std::uint32_t value = bits(static_cast<float>(x));
                    if (l == 0 && y == 1) {
                        stored = {value, 0, 0, bits(0.5f)};  // green and blue get 0 (8.2.4)
                    } else if (l == slice && y == 2) {
                        stored = {value, bits(2.0f), bits(4.0f), 0};
                    }
                }
                std::array<std::uint32_t, 4> found;
                std::memcpy(found.data(),
                            memory.data() + ((std::size_t{l} * side + y) * side + x) * 16, 16);
                EXPECT_EQ(found, stored);
            }
        }
    }
}

TEST_F(ImageRender, StoreTwoD) { check_store(*runner_, {}, 1); }
TEST_F(ImageRender, StoreArray) { check_store(*runner_, {.type = 13}, 2); }
TEST_F(ImageRender, StoreUint) {
    check_store(*runner_, {.number = ImageClass::Number::uint}, 1, VK_FORMAT_R32G32B32A32_UINT);
}
TEST_F(ImageRender, StoreSint) {
    check_store(*runner_, {.number = ImageClass::Number::sint}, 1, VK_FORMAT_R32G32B32A32_SINT);
}

}  // namespace
