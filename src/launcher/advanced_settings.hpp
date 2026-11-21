// The choice of the Advanced page and its mapping onto the config file (docs/design/launcher.md,
// "Advanced"). Pure, so tests/launcher covers it.
#pragma once

#include <cmath>
#include <optional>

#include "common/config.hpp"

namespace bb::launcher {

// The load hold slider: whole seconds from 0 to the game's 12 (#704).
inline constexpr int game_load_hold = static_cast<int>(common::max_load_hold);

// What the slider shows for `config`: the hold in whole seconds, the game's 12 without a key. A
// fraction in the file shows rounded and stays until the slider moves.
inline int load_hold_choice(const common::Config& config) {
    return static_cast<int>(std::lround(config.load_hold.value_or(common::max_load_hold)));
}

// Puts a moved slider into `config`: the game's 12 removes the key, anything else is written.
// A slider the player left alone writes nothing.
inline void apply_load_hold(common::Config& config, int before, int after) {
    if (after != before) {
        config.load_hold = after == game_load_hold ? std::nullopt
                                                   : std::optional(static_cast<float>(after));
    }
}

}  // namespace bb::launcher
