#include "launcher/advanced_settings.hpp"

#include <gtest/gtest.h>

namespace {

using namespace bb::launcher;
using bb::common::Config;

TEST(AdvancedSettings, NoKeyIsTheGamesHold) {
    EXPECT_EQ(load_hold_choice({}), 12);
    Config config;
    apply_load_hold(config, 12, 12);
    EXPECT_EQ(config, Config{});
}

TEST(AdvancedSettings, MapsTheLoadHoldOntoItsKey) {
    Config config;
    apply_load_hold(config, 12, 0);
    EXPECT_EQ(config.load_hold, 0.0f);
    EXPECT_EQ(load_hold_choice(config), 0);
    EXPECT_EQ(bb::common::format_config(config), "load_hold = 0\n");
    apply_load_hold(config, 0, 5);
    EXPECT_EQ(config.load_hold, 5.0f);
    // The game's 12 again removes the key.
    apply_load_hold(config, 5, 12);
    EXPECT_FALSE(config.load_hold);
}

TEST(AdvancedSettings, AFractionSetByHandStaysUntilTheSliderMoves) {
    Config config;
    config.load_hold = 2.5f;
    const int shown = load_hold_choice(config);
    EXPECT_EQ(shown, 3);
    apply_load_hold(config, shown, shown);
    EXPECT_EQ(config.load_hold, 2.5f);
    apply_load_hold(config, shown, 2);
    EXPECT_EQ(config.load_hold, 2.0f);
}

}  // namespace
