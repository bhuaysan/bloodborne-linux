// Tests of bb-run's step from its options to the state of the game hooks and the eboot
// (src/tools/game_options.cpp, #725): each option reaches its setter, and the eboot's edits
// follow the resolution.
#include "tools/game_options.hpp"

#include <array>
#include <cstdint>
#include <cstring>

#include <gtest/gtest.h>

#include "game/camera/aspect.hpp"
#include "game/camera/field_of_view.hpp"
#include "game/frame/frame.hpp"
#include "game/gfx/frame_buffer.hpp"
#include "game/gfx/hud_stage.hpp"
#include "game/gfx/post_effects.hpp"
#include "game/image.hpp"
#include "game/load/load_hold.hpp"
#include "harness/eboot.hpp"

namespace {

using bb::tools::RunOptions;
using bb::test::EbootTest;

// Gives every setting its default back, as bb-run's defaults set them.
struct DefaultsGuard {
    ~DefaultsGuard() {
        bb::tools::prepare_eboot(bb::game::image(), RunOptions{});
        bb::tools::apply_game_options(RunOptions{});
    }
};

// Options whose values all differ from the defaults and from each other.
RunOptions changed_options() {
    RunOptions o;
    o.resolution_width = 5120;
    o.resolution_height = 1440;
    o.frame_rate = 120;
    o.load_hold = 3.5f;
    o.motion_blur = false;
    o.motion_blur_strength = 0.25f;
    o.chromatic_aberration = true;
    o.chromatic_aberration_strength = 0.75f;
    o.fov = 58.0f;
    return o;
}

TEST_F(EbootTest, GameOptionsReachTheirSetters) {
    DefaultsGuard guard;
    const RunOptions o = changed_options();
    bb::tools::apply_game_options(o);
    EXPECT_EQ(bb::game::global<std::uint32_t>(bb::game::frame_buffer_width), 5120U);
    EXPECT_EQ(bb::game::global<std::uint32_t>(bb::game::frame_buffer_height), 1440U);
    EXPECT_FLOAT_EQ(bb::game::screen_aspect(), 32.0f / 9.0f);
    EXPECT_EQ(bb::game::frame_rate(), 120U);
    EXPECT_EQ(bb::game::load_hold(), 3.5f);
    const bb::game::PostEffects& fx = bb::game::post_effects();
    EXPECT_FALSE(fx.motion_blur);
    EXPECT_EQ(fx.motion_blur_strength, 0.25f);
    EXPECT_TRUE(fx.chromatic_aberration);
    EXPECT_EQ(fx.chromatic_aberration_strength, 0.75f);
    EXPECT_EQ(bb::game::field_of_view(), 58.0f);

    // The other way round for the switches.
    RunOptions swapped = o;
    swapped.motion_blur = true;
    swapped.chromatic_aberration = false;
    bb::tools::apply_game_options(swapped);
    EXPECT_TRUE(bb::game::post_effects().motion_blur);
    EXPECT_FALSE(bb::game::post_effects().chromatic_aberration);
}

TEST_F(EbootTest, TheDefaultsAreTheGamesState) {
    DefaultsGuard guard;
    bb::tools::apply_game_options(changed_options());
    bb::tools::apply_game_options(RunOptions{});
    EXPECT_EQ(bb::game::global<std::uint32_t>(bb::game::frame_buffer_width), 1920U);
    EXPECT_EQ(bb::game::global<std::uint32_t>(bb::game::frame_buffer_height), 1080U);
    EXPECT_EQ(bb::game::screen_aspect(), bb::game::game_aspect);
    EXPECT_EQ(bb::game::frame_rate(), bb::game::default_frame_rate);
    EXPECT_EQ(bb::game::load_hold(), bb::game::game_load_hold);
    EXPECT_EQ(bb::game::post_effects(), bb::game::PostEffects{});
    EXPECT_EQ(bb::game::field_of_view(), bb::game::game_fov);
}

TEST_F(EbootTest, PrepareEbootFollowsTheResolution) {
    DefaultsGuard guard;
    bb::tools::prepare_eboot(image(), changed_options());
    EXPECT_EQ(bb::game::global<std::uint64_t>(bb::game::graphics_private_heap_size_entry),
              bb::game::graphics_private_heap_size(5120, 1440));
    EXPECT_TRUE((bb::game::global<std::array<float, 4>>(bb::game::device_clear_color) ==
                 bb::game::device_clear_black));
    for (const bb::game::HudSizeLoad& load : bb::game::hud_size_loads) {
        const auto want = bb::game::stage_size_load(load);
        EXPECT_EQ(std::memcmp(reinterpret_cast<const void*>(image().runtime(load.link)),
                              want.data(), want.size()),
                  0)
            << std::hex << load.link;
    }

    bb::tools::prepare_eboot(image(), RunOptions{});
    EXPECT_EQ(bb::game::global<std::uint64_t>(bb::game::graphics_private_heap_size_entry),
              bb::game::graphics_private_heap_default);
    EXPECT_TRUE((bb::game::global<std::array<float, 4>>(bb::game::device_clear_color) ==
                 bb::game::device_clear_grey));
    for (const bb::game::HudSizeLoad& load : bb::game::hud_size_loads) {
        const auto want = bb::game::game_size_load(load);
        EXPECT_EQ(std::memcmp(reinterpret_cast<const void*>(image().runtime(load.link)),
                              want.data(), want.size()),
                  0)
            << std::hex << load.link;
    }
}

}  // namespace
