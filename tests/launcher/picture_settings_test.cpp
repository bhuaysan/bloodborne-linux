#include "launcher/picture_settings.hpp"

#include <gtest/gtest.h>

namespace {

using namespace bb::launcher;
using bb::common::Config;

TEST(PictureSettings, AnEmptyConfigShowsBbRunsDefaults) {
    const PictureChoices c = choices_from_config({});
    EXPECT_FALSE(c.fullscreen);
    EXPECT_EQ(c.size, (Size{1280, 720}));
    EXPECT_EQ(c.frame_rate, 30u);
    EXPECT_EQ(c.sharpness, Sharpness::normal);
}

TEST(PictureSettings, ChoicesGoToTheConfigAndComeBack) {
    for (const bool fullscreen : {false, true}) {
        for (const unsigned rate : {30u, 60u, 120u}) {
            for (const Sharpness sharp :
                 {Sharpness::normal, Sharpness::sharpened, Sharpness::extra_sharp}) {
                const PictureChoices want{fullscreen, {1920, 1080}, rate, sharp};
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
