// The step from bb-run's options to the state of the game hooks and the eboot (#725), a function
// of its own so that a test can check that each option reaches its setter.
#pragma once

#include "loader/image.hpp"
#include "tools/run_options.hpp"

namespace bb::tools {

// The eboot's edits for the frame buffer size: the GFX_GraphicsPrivate heap size, the HUD's
// stage loads and the device clear colour (game/gfx). Called on the mapped eboot before its entry
// point runs and before it is set as game::image().
void prepare_eboot(loader::Image& eboot, const RunOptions& options);

// The frame buffer size globals of game::image() and the settings of the game's hooks: the
// screen aspect, frame rate, loading page hold, post effects and field of view. Called once
// game::image() is set and before the entry point runs. Throws std::invalid_argument for a value
// a setter refuses (parse_run_options refuses them first).
void apply_game_options(const RunOptions& options);

}  // namespace bb::tools
