// bb-run's field-of-view option (#666; docs/design/image-quality.md, "Camera and aspect ratio"):
// the player's chase camera holds a wider vertical field of view between its updates, so the copy
// the game makes of it for rendering, and with it the projection, the culling, the shadow
// cascades and YEBIS, see the wider angle. Cutscene and other cameras keep their own.
#pragma once

#include <cstdint>

namespace bb::game {

// The chase camera's vertical field of view in radians. Its update (0x0143ac60) eases it toward
// the angle of its camera parameter row, clamped to 38 to 48 degrees, and reads it for its
// collision and distance (0x0143bbb0, 0x0143bf04).
inline constexpr std::uint32_t chase_cam_fov = 0x50;

// The chase camera's angle in its default parameter row and its constructor, in degrees. --fov
// names the angle that takes its place; other rows keep their difference to it.
inline constexpr float game_fov = 43.0f;
// The range of --fov. The game's rows go up to 48 degrees, so 65 shows at most 70, the most the
// #666 runs checked; #660 saw streaks over the picture at 77.
inline constexpr float min_fov = 30.0f;
inline constexpr float max_fov = 65.0f;

// Sets the field of view in degrees, min_fov to max_fov (NaN or outside throws
// std::invalid_argument). game_fov leaves the game's angles alone. Called before the game runs;
// not thread-safe.
void set_field_of_view(float degrees);
float field_of_view();

// `fov` (radians) moved by `degrees` - game_fov.
float widened_fov(float fov, float degrees);

// The widening of one chase camera's +0x50 around its updates. The game's own value is in the
// field during the update, so its easing and collision work as without the option; between
// updates the field holds the widened value.
class ChaseFovWidener {
public:
    // Before the update: puts the game's value back, unless something else wrote the field since
    // widen (then that value is the game's).
    void restore(void* cam);
    // After the update: keeps the game's value and writes it widened to `degrees`; at game_fov
    // it leaves the field alone.
    void widen(void* cam, float degrees);

private:
    const void* cam_ = nullptr;
    float game_ = 0.0f;
    float widened_ = 0.0f;
};

// The widener of the chase camera update wrap (chase_cam.cpp), on the game thread.
ChaseFovWidener& chase_fov_widener();

}  // namespace bb::game
