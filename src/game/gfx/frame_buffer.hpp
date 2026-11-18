// The engine's frame buffer size, which bb-run --resolution sets before the game starts
// (docs/design/resolution.md, "Decision: way B").
#pragma once

#include <cstdint>

namespace bb::game {

// The engine sizes every screen-sized render target, the post-processing chain, the compute grids,
// the viewports and the display buffers from these two globals. Their data defaults are 1920 and
// 1080; system_properties_init (0x02017770) reads them as the fallbacks of its configuration keys
// and writes them back, and graphics_init (0x01d969f0) builds the graphics device from them.
inline constexpr std::uint64_t frame_buffer_width = 0x051289f8;
inline constexpr std::uint64_t frame_buffer_height = 0x051289fc;

// Writes `width` and `height` to the two globals of image(). Called once after the eboot is
// mapped and before its entry point runs, so every reader sees the new size. Unlike the game's
// own setter of the same name (0x01d9e850) it leaves the HUD scale factors (0x055404d0,
// 0x055404d4) alone: system_properties_init computes them from the globals later. Writing the
// globals once the game runs would leave those factors and the allocated targets at the old size.
void set_frame_buffer_size(std::uint32_t width, std::uint32_t height);

}  // namespace bb::game
