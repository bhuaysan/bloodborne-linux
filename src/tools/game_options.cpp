// The step from bb-run's options to the state of the game hooks and the eboot (#725).
#include "tools/game_options.hpp"

#include "game/camera/aspect.hpp"
#include "game/camera/field_of_view.hpp"
#include "game/frame/frame.hpp"
#include "game/gfx/frame_buffer.hpp"
#include "game/gfx/hud_stage.hpp"
#include "game/gfx/post_effects.hpp"
#include "game/load/load_hold.hpp"

namespace bb::tools {

static_assert(max_load_hold == game::game_load_hold,
              "--load-hold's range must end at the game's hold");
static_assert(common::max_post_effect_strength == game::max_post_effect_strength);
static_assert(common::min_fov == game::min_fov && common::max_fov == game::max_fov);
static_assert(common::game_fov == game::game_fov, "--fov's default must be the game's angle");

void prepare_eboot(loader::Image& eboot, const RunOptions& options) {
    // The engine sets its heaps up at start, and the larger targets of a larger frame buffer
    // come out of GFX_GraphicsPrivate (docs/design/resolution.md).
    game::set_graphics_heap_size(eboot, options.resolution_width, options.resolution_height);
    // The HUD places its world markers in 1920x1080 stage coordinates (#713).
    game::set_hud_stage_loads(eboot, options.resolution_width, options.resolution_height);
    // Black, not the game's grey, beside the 16:9 menus at a wider frame buffer (#711).
    game::set_device_clear_color(eboot, options.resolution_width, options.resolution_height);
}

void apply_game_options(const RunOptions& options) {
    // system_properties_init and the graphics device read the size from the globals
    // (docs/design/resolution.md).
    game::set_frame_buffer_size(options.resolution_width, options.resolution_height);
    game::set_screen_aspect(options.resolution_width, options.resolution_height);
    game::set_frame_rate(options.frame_rate);
    game::set_load_hold(options.load_hold);
    game::set_post_effects({.motion_blur = options.motion_blur,
                            .motion_blur_strength = options.motion_blur_strength,
                            .chromatic_aberration = options.chromatic_aberration,
                            .chromatic_aberration_strength =
                                options.chromatic_aberration_strength});
    game::set_field_of_view(options.fov);
}

}  // namespace bb::tools
