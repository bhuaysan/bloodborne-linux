// The sizes of the game's frame and of the presented frame, written by the presenter. ImeDialog
// sizes its window with them.
#pragma once

#include <utility>

#include "common/types.h"

struct DisplayStateImpl {
    std::pair<u32, u32> game_resolution{};
    std::pair<u32, u32> output_resolution{};
};

inline DisplayStateImpl DisplayState;
