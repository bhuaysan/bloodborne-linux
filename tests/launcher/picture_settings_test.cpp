#include "launcher/picture_settings.hpp"

#include <gtest/gtest.h>

#include <algorithm>

namespace {

using namespace bb::launcher;
using bb::common::Config;

TEST(PictureSettings, AnEmptyConfigShowsBbRunsDefaults) {
    const PictureChoices c = choices_from_config({});
    EXPECT_FALSE(c.fullscreen);
    EXPECT_EQ(c.size, (Size{1280, 720}));
    EXPECT_EQ(c.frame_rate, 30u);
    EXPECT_EQ(c.sharpness, Sharpness::normal);
    EXPECT_FALSE(c.wide);
    EXPECT_EQ(c.anisotropy, 0u);
    EXPECT_EQ(c.fov, 43.0f);
    EXPECT_TRUE(c.motion_blur);
    EXPECT_TRUE(c.chromatic_aberration);
}

// The defaults write nothing: the page leaves the file as it was.
TEST(PictureSettings, TheDefaultsLeaveTheConfigAlone) {
    Config config;
    const PictureChoices c = choices_from_config(config);
    apply_choices(config, c, c);
    EXPECT_EQ(config, Config{});
    // Choosing something and then the default again within one visit writes nothing either.
    PictureChoices round = c;
    round.anisotropy = 16;
    round.anisotropy = 0;
    apply_choices(config, c, round);
    EXPECT_EQ(config, Config{});
}

TEST(PictureSettings, R11ChoicesGoToTheConfigAndComeBack) {
    for (const float fov : {bb::common::game_fov, bb::common::min_fov, 52.0f, bb::common::max_fov}) {
        for (const unsigned level : anisotropy_choices) {
            for (const bool blur : {false, true}) {
                for (const bool fringes : {false, true}) {
                    PictureChoices want;
                    want.fov = fov;
                    want.anisotropy = level;
                    want.motion_blur = blur;
                    want.chromatic_aberration = fringes;
                    Config config;
                    apply_choices(config, choices_from_config(config), want);
                    EXPECT_EQ(choices_from_config(config), want);
                    // And the text of the file reads back the same.
                    EXPECT_EQ(bb::common::parse_config(bb::common::format_config(config), "c")
                                  .config,
                              config);
                }
            }
        }
    }
}

TEST(PictureSettings, MapsTheR11ChoicesOntoTheirKeys) {
    Config config;
    const PictureChoices base;
    PictureChoices c = base;
    c.fov = 60.0f;
    c.anisotropy = 16;
    c.motion_blur = false;
    c.chromatic_aberration = false;
    apply_choices(config, base, c);
    EXPECT_EQ(config.fov, 60.0f);
    EXPECT_EQ(config.anisotropy, 16u);
    EXPECT_EQ(config.motion_blur, false);
    EXPECT_EQ(config.chromatic_aberration, false);
    // Back to the game's view and filtering removes the keys; bb-run has no "game" anisotropy key.
    apply_choices(config, c, base);
    EXPECT_FALSE(config.fov);
    EXPECT_FALSE(config.anisotropy);
    EXPECT_EQ(config.motion_blur, true);
    EXPECT_EQ(config.chromatic_aberration, true);
}

TEST(PictureSettings, OddR11ValuesAndStrengthsSurvive) {
    Config config;
    config.fov = 47.5f;
    config.anisotropy = 1;  // a level the list has no entry for
    config.motion_blur_strength = 0.5f;
    config.chromatic_aberration_strength = 0.25f;
    config.resolution = std::pair{2560u, 1440u};  // a 16:9 frame set by hand
    const PictureChoices before = choices_from_config(config);
    EXPECT_EQ(before.fov, 47.5f);
    EXPECT_EQ(before.anisotropy, 1u);
    EXPECT_FALSE(before.wide);
    PictureChoices after = before;
    after.motion_blur = false;
    apply_choices(config, before, after);
    EXPECT_EQ(config.fov, 47.5f);
    EXPECT_EQ(config.anisotropy, 1u);
    EXPECT_EQ(config.motion_blur_strength, 0.5f);
    EXPECT_EQ(config.chromatic_aberration_strength, 0.25f);
    EXPECT_EQ(config.resolution, (Size{2560, 1440}));
    EXPECT_EQ(config.motion_blur, false);
}

TEST(PictureSettings, DisplayShapes) {
    EXPECT_EQ(shape_of({1920, 1080}), Shape::normal);
    EXPECT_EQ(shape_of({1920, 1200}), Shape::normal);
    EXPECT_EQ(shape_of({0, 0}), Shape::normal);
    EXPECT_EQ(shape_of({2560, 1080}), Shape::wide);
    EXPECT_EQ(shape_of({3440, 1440}), Shape::wide);
    EXPECT_EQ(shape_of({3840, 1600}), Shape::wide);
    EXPECT_EQ(shape_of({5120, 1440}), Shape::super_wide);
    EXPECT_EQ(shape_of({3840, 1080}), Shape::super_wide);
}

TEST(PictureSettings, WideSizesOnlyOnAWideDisplay) {
    EXPECT_EQ(resolution_choices({3440, 1440}, {1280, 720}),
              (std::vector<Size>{{1280, 720},
                                 {1600, 900},
                                 {1920, 1080},
                                 {2560, 1080},
                                 {2560, 1440},
                                 {3440, 1440}}));
    EXPECT_EQ(resolution_choices({2560, 1080}, {1280, 720}),
              (std::vector<Size>{{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1080}}));
    // 32:9: its own size, not the 21:9 ones.
    EXPECT_EQ(resolution_choices({5120, 1440}, {1280, 720}),
              (std::vector<Size>{
                  {1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {5120, 1440}}));
    // A 16:9 display gets no wide size, also when it is large enough.
    const auto list = resolution_choices({5120, 2880}, {1280, 720});
    EXPECT_EQ(std::count(list.begin(), list.end(), Size{2560, 1080}), 0);
    EXPECT_EQ(std::count(list.begin(), list.end(), Size{5120, 1440}), 0);
}

TEST(PictureSettings, AWideWindowRendersAWideFrame) {
    Config config;
    const PictureChoices before = choices_from_config(config);
    PictureChoices after = before;
    after.size = {3440, 1440};
    after.wide = wide_frame(after, {3440, 1440});
    EXPECT_EQ(after.wide, (Size{3440, 1440}));
    apply_choices(config, before, after);
    EXPECT_EQ(config.window_size, (Size{3440, 1440}));
    EXPECT_EQ(config.resolution, (Size{3440, 1440}));
    EXPECT_EQ(choices_from_config(config), after);
    // A 16:9 window again: bb-run's 16:9 frame, the key goes.
    PictureChoices back = after;
    back.size = {1920, 1080};
    back.wide = wide_frame(back, {3440, 1440});
    EXPECT_FALSE(back.wide);
    apply_choices(config, after, back);
    EXPECT_FALSE(config.resolution);
    EXPECT_EQ(config.window_size, (Size{1920, 1080}));
}

TEST(PictureSettings, FullscreenRendersAtTheDisplaysShape) {
    PictureChoices c;
    c.fullscreen = true;
    EXPECT_FALSE(wide_frame(c, {1920, 1080}));
    EXPECT_FALSE(wide_frame(c, {0, 0}));
    EXPECT_EQ(wide_frame(c, {2560, 1080}), (Size{2560, 1080}));
    EXPECT_EQ(wide_frame(c, {3440, 1440}), (Size{3440, 1440}));
    EXPECT_EQ(wide_frame(c, {3840, 1600}), (Size{3440, 1440}));  // the largest 21:9 size
    EXPECT_EQ(wide_frame(c, {5120, 1440}), (Size{5120, 1440}));
    EXPECT_EQ(wide_frame(c, {3840, 1080}), (Size{5120, 1440}));
    // The window size does not count in fullscreen.
    c.size = {2560, 1080};
    EXPECT_FALSE(wide_frame(c, {1920, 1080}));
    c.fullscreen = false;
    EXPECT_EQ(wide_frame(c, {1920, 1080}), (Size{2560, 1080}));
}

TEST(PictureSettings, ChoicesGoToTheConfigAndComeBack) {
    for (const bool fullscreen : {false, true}) {
        for (const unsigned rate : {30u, 60u, 120u}) {
            for (const Sharpness sharp :
                 {Sharpness::normal, Sharpness::sharpened, Sharpness::extra_sharp}) {
                PictureChoices want;
                want.fullscreen = fullscreen;
                want.size = {1920, 1080};
                want.frame_rate = rate;
                want.sharpness = sharp;
                Config config;
                apply_choices(config, choices_from_config(config), want);
                EXPECT_EQ(choices_from_config(config), want);
            }
        }
    }
}

TEST(PictureSettings, MapsSharpnessOntoFsrAndRcas) {
    Config config;
    PictureChoices base;
    PictureChoices c = base;
    c.sharpness = Sharpness::sharpened;
    apply_choices(config, base, c);
    EXPECT_EQ(config.fsr, true);
    EXPECT_FALSE(config.rcas_attenuation);  // RCAS at its default attenuation

    PictureChoices d = c;
    d.sharpness = Sharpness::extra_sharp;
    apply_choices(config, c, d);
    EXPECT_EQ(config.fsr, true);
    EXPECT_EQ(config.rcas_attenuation, 0.0f);

    apply_choices(config, d, base);
    EXPECT_EQ(config.fsr, false);
    EXPECT_FALSE(config.rcas_attenuation);  // bb-run refuses RCAS keys without fsr
    EXPECT_FALSE(config.rcas);
}

TEST(PictureSettings, SettingsThePageDidntWriteSurvive) {
    Config config;
    config.game_dir = "/games/bb";
    config.profile = "test";
    config.window_size = {1366, 768};  // not in the list
    config.fsr = true;
    config.rcas_attenuation = 0.5f;
    const PictureChoices before = choices_from_config(config);
    EXPECT_EQ(before.size, (Size{1366, 768}));
    EXPECT_EQ(before.sharpness, Sharpness::sharpened);

    PictureChoices after = before;
    after.frame_rate = 60;  // only the frame rate changes
    apply_choices(config, before, after);
    EXPECT_EQ(config.window_size, (Size{1366, 768}));
    EXPECT_EQ(config.rcas_attenuation, 0.5f);
    EXPECT_EQ(config.fsr, true);
    EXPECT_EQ(config.frame_rate, 60u);
    EXPECT_EQ(config.game_dir, "/games/bb");
    EXPECT_EQ(config.profile, "test");
}

TEST(PictureSettings, NothingChangedWritesNothing) {
    Config config;
    config.rcas = false;
    config.fsr = true;
    const Config original = config;
    const PictureChoices c = choices_from_config(config);
    apply_choices(config, c, c);
    EXPECT_EQ(config, original);
}

TEST(PictureSettings, FullscreenKeepsTheWindowSize) {
    Config config;
    config.window_size = {1600, 900};
    const PictureChoices before = choices_from_config(config);
    PictureChoices after = before;
    after.fullscreen = true;
    apply_choices(config, before, after);
    EXPECT_EQ(config.fullscreen, true);
    EXPECT_EQ(config.window_size, (Size{1600, 900}));
}

TEST(PictureSettings, ResolutionListStopsAtTheDisplay) {
    const auto list = resolution_choices({2560, 1440}, {1280, 720});
    ASSERT_EQ(list.size(), 4u);
    EXPECT_EQ(list.back(), (Size{2560, 1440}));
    // A small display still offers the default.
    EXPECT_EQ(resolution_choices({1024, 600}, {1280, 720}), (std::vector<Size>{{1280, 720}}));
    // An unknown display (0 x 0) too.
    EXPECT_EQ(resolution_choices({0, 0}, {1280, 720}).size(), 1u);
}

TEST(PictureSettings, AnOddSizeIsListedInOrder) {
    const auto list = resolution_choices({1920, 1080}, {1366, 768});
    EXPECT_EQ(list, (std::vector<Size>{{1280, 720}, {1366, 768}, {1600, 900}, {1920, 1080}}));
    // A size above the display is kept as well.
    EXPECT_EQ(resolution_choices({1920, 1080}, {3840, 2160}).back(), (Size{3840, 2160}));
}

}  // namespace
