// The aspect ratio of the game's projection at a frame buffer wider than 16:9 (#667). The wrap of
// the render camera copy is the one place every camera passes: the chase camera, lock-on and
// cutscene cameras reach the render camera only through it (#660, #666), so culling, the shadow
// cascades and YEBIS, which read the same render camera, follow.
#include "game/camera/aspect.hpp"

#include "game/image.hpp"
#include "hook/hook.hpp"

namespace bb::game {
namespace {

float aspect_now = game_aspect;

}  // namespace

void set_screen_aspect(std::uint32_t width, std::uint32_t height) {
    const bool sixteen_nine = std::uint64_t{width} * 9 == std::uint64_t{height} * 16;
    aspect_now = sixteen_nine || height == 0 ? game_aspect
                                             : static_cast<float>(width) / static_cast<float>(height);
}

float screen_aspect() {
    return aspect_now;
}

float wide_aspect(float aspect, float screen) {
    return aspect == game_aspect ? screen : aspect;
}

using RenderCamCopy = void(const void* game_cam, void* render_cam);

// 0x01d16690
void render_cam_copy_wide(const void* game_cam, void* render_cam) {
    reinterpret_cast<RenderCamCopy*>(hook::original(image(), render_cam_copy))(game_cam,
                                                                                render_cam);
    const float screen = aspect_now;
    if (screen != game_aspect) {
        store<float>(render_cam, render_cam_aspect,
                     wide_aspect(load<float>(render_cam, render_cam_aspect), screen));
    }
}

}  // namespace bb::game

BB_WRAP(bb::game::render_cam_copy, bb::game::render_cam_copy_wide,
        bb::game::render_cam_copy_prologue);
