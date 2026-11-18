// The engine's frame buffer size (docs/design/resolution.md, "Decision: way B").
#include "game/gfx/frame_buffer.hpp"

#include "game/image.hpp"

namespace bb::game {

void set_frame_buffer_size(std::uint32_t width, std::uint32_t height) {
    global<std::uint32_t>(frame_buffer_width) = width;
    global<std::uint32_t>(frame_buffer_height) = height;
}

}  // namespace bb::game
