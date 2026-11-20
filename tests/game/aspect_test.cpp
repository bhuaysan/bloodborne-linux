// Tests of the wide aspect in src/game/camera/aspect.cpp (#667): the aspect for each frame buffer
// size, and, against the eboot, the game's render camera copy (0x01d16690) under the wrap.
#include "game/camera/aspect.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <gtest/gtest.h>

#include "game/image.hpp"
#include "harness/eboot.hpp"
#include "hook/hook.hpp"

namespace {

using bb::game::game_aspect;
using bb::game::render_cam_copy;
using bb::test::EbootTest;

struct AspectGuard {
    ~AspectGuard() { bb::game::set_screen_aspect(1920, 1080); }
};

TEST(WideAspect, IsTheGamesConstant) {
    EXPECT_EQ(std::bit_cast<std::uint32_t>(game_aspect), 0x3fe38e39U);
}

TEST(WideAspect, SixteenNineSizesKeepTheGamesAspect) {
    AspectGuard guard;
    for (const auto& [w, h] : {std::pair{1920U, 1080U}, std::pair{2560U, 1440U},
                               std::pair{3200U, 1800U}, std::pair{3840U, 2160U}}) {
        bb::game::set_screen_aspect(w, h);
        // Bit for bit: 2560.0f / 1440.0f need not round to the game's constant.
        EXPECT_EQ(std::bit_cast<std::uint32_t>(bb::game::screen_aspect()), 0x3fe38e39U) << w;
    }
}

TEST(WideAspect, WideSizesTakeTheirOwn) {
    AspectGuard guard;
    bb::game::set_screen_aspect(2560, 1080);
    EXPECT_FLOAT_EQ(bb::game::screen_aspect(), 64.0f / 27.0f);
    bb::game::set_screen_aspect(3440, 1440);
    EXPECT_FLOAT_EQ(bb::game::screen_aspect(), 43.0f / 18.0f);
    bb::game::set_screen_aspect(5120, 1440);
    EXPECT_FLOAT_EQ(bb::game::screen_aspect(), 32.0f / 9.0f);
    bb::game::set_screen_aspect(1920, 1080);
    EXPECT_EQ(bb::game::screen_aspect(), game_aspect);
}

TEST(WideAspect, OnlyTheGames16x9IsReplaced) {
    const float wide = 64.0f / 27.0f;
    EXPECT_EQ(bb::game::wide_aspect(game_aspect, wide), wide);
    EXPECT_EQ(bb::game::wide_aspect(1.0f, wide), 1.0f);  // the character editor's preview
    EXPECT_EQ(bb::game::wide_aspect(0.0f, wide), 0.0f);  // width / height of the render camera
    EXPECT_EQ(bb::game::wide_aspect(1.7777f, wide), 1.7777f);
    EXPECT_EQ(bb::game::wide_aspect(game_aspect, game_aspect), game_aspect);
}

using AspectGame = EbootTest;

const std::uint8_t* at(std::uint64_t link) {
    return reinterpret_cast<const std::uint8_t*>(bb::game::image().runtime(link));
}

TEST_F(AspectGame, TheCamerasStoreTheConstant) {
    // SprjPersCam's constructor: movabs rax, (aspect << 32) | fov, stored to +0x50.
    constexpr std::array<std::uint8_t, 10> base = {0x48, 0xb8, 0x37, 0x20, 0x40, 0x3f,
                                                   0x39, 0x8e, 0xe3, 0x3f};
    EXPECT_EQ(std::memcmp(at(0x01d1662c), base.data(), base.size()), 0);
    // The chase camera's constructor: mov dword [rbx+0x54], 0x3fe38e39.
    constexpr std::array<std::uint8_t, 7> chase = {0xc7, 0x43, 0x54, 0x39, 0x8e, 0xe3, 0x3f};
    EXPECT_EQ(std::memcmp(at(0x0143a35a), chase.data(), chase.size()), 0);
    EXPECT_EQ(std::memcmp(at(render_cam_copy), bb::game::render_cam_copy_prologue.data(),
                          bb::game::render_cam_copy_prologue.size()),
              0);
}

struct alignas(16) Cam {
    std::array<std::byte, 0x90> bytes{};
    float get(std::uint32_t offset) const { return bb::game::load<float>(bytes.data(), offset); }
    void set(std::uint32_t offset, float v) { bb::game::store<float>(bytes.data(), offset, v); }
};

// The game's copy, run through the registered wrap.
class Wrapped {
public:
    Wrapped() {
        for (const bb::hook::Replacement& r : bb::hook::replacements()) {
            if (r.link_addr == render_cam_copy) {
                hooks_[0] = r;
            }
        }
        EXPECT_EQ(hooks_[0].link_addr, render_cam_copy);
        bb::hook::apply(bb::game::image(), hooks_);
    }
    ~Wrapped() { bb::hook::remove(bb::game::image(), hooks_); }
    Wrapped(const Wrapped&) = delete;
    Wrapped& operator=(const Wrapped&) = delete;

    static Cam copy(const Cam& game_cam) {
        Cam render;
        render.set(bb::game::render_cam_aspect, -1.0f);
        reinterpret_cast<void (*)(const void*, void*)>(bb::game::image().runtime(render_cam_copy))(
            game_cam.bytes.data(), render.bytes.data());
        return render;
    }

private:
    std::array<bb::hook::Replacement, 1> hooks_{};
};

Cam game_camera(float aspect) {
    Cam c;
    for (std::uint32_t i = 0; i < 16; ++i) {
        c.set(0x10 + 4 * i, static_cast<float>(i + 1));  // the matrix
    }
    c.set(0x50, 0.75f);  // fov
    c.set(bb::game::game_cam_aspect, aspect);
    c.set(0x58, 0.05f);  // near
    c.set(0x5c, 3000.0f);  // far
    return c;
}

TEST_F(AspectGame, TheCopyTakesTheScreensAspect) {
    AspectGuard guard;
    Wrapped wrap;
    const Cam game = game_camera(game_aspect);

    Cam render = Wrapped::copy(game);
    EXPECT_EQ(render.get(bb::game::render_cam_aspect), game_aspect);
    EXPECT_EQ(render.get(0x00), 1.0f);
    EXPECT_EQ(render.get(0x3c), 16.0f);
    EXPECT_EQ(render.get(0x50), 0.75f);
    EXPECT_EQ(render.get(0x54), 0.05f);
    EXPECT_EQ(render.get(0x58), 3000.0f);
    EXPECT_EQ(bb::game::load<std::uint32_t>(render.bytes.data(), 0x5c), 1920U);
    EXPECT_EQ(bb::game::load<std::uint32_t>(render.bytes.data(), 0x60), 1080U);

    bb::game::set_screen_aspect(2560, 1080);
    render = Wrapped::copy(game);
    EXPECT_FLOAT_EQ(render.get(bb::game::render_cam_aspect), 64.0f / 27.0f);
    EXPECT_EQ(render.get(0x50), 0.75f);  // the vertical angle stays
    EXPECT_EQ(bb::game::load<std::uint32_t>(render.bytes.data(), 0x5c), 1920U);

    // Other aspects pass through.
    EXPECT_EQ(Wrapped::copy(game_camera(1.0f)).get(bb::game::render_cam_aspect), 1.0f);
    EXPECT_EQ(Wrapped::copy(game_camera(0.0f)).get(bb::game::render_cam_aspect), 0.0f);
}

}  // namespace
