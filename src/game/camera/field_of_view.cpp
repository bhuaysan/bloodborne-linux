// bb-run's field-of-view option (#666; docs/design/image-quality.md, "Camera and aspect ratio").
// The angle can't be set once: the chase camera's update eases +0x50 back toward its parameter
// row every call. Widening it after each update and restoring it before the next holds, and
// never adds up. The render camera is not the place either: the game copies the chase camera
// into a SprjPersCam (vtable 0x0539ffb0) before 0x01d16690 copies that into the render camera,
// as it does with cutscene cameras, so there the two can't be told apart.
#include "game/camera/field_of_view.hpp"

#include <cmath>
#include <format>
#include <numbers>
#include <stdexcept>

#include "game/image.hpp"

namespace bb::game {
namespace {

float fov_degrees = game_fov;

}  // namespace

void set_field_of_view(float degrees) {
    // The negated test also refuses NaN.
    if (!(degrees >= min_fov && degrees <= max_fov)) {
        throw std::invalid_argument(std::format("field of view {} is not from {} to {} degrees",
                                                degrees, min_fov, max_fov));
    }
    fov_degrees = degrees;
}

float field_of_view() {
    return fov_degrees;
}

float horizontal_fov(float degrees, std::uint32_t width, std::uint32_t height) {
    const double half = static_cast<double>(degrees) * std::numbers::pi / 360.0;
    const double aspect = static_cast<double>(width) / static_cast<double>(height);
    return static_cast<float>(std::atan(std::tan(half) * aspect) * 360.0 / std::numbers::pi);
}

float widened_fov(float fov, float degrees) {
    return fov + (degrees - game_fov) * (std::numbers::pi_v<float> / 180.0f);
}

void ChaseFovWidener::restore() {
    // Also before another camera's update: the one widened last gets its value back, so that its
    // next update eases from the game's angle and doesn't widen the widened one again.
    if (cam_ != nullptr && load<float>(cam_, chase_cam_fov) == widened_) {
        store<float>(cam_, chase_cam_fov, game_);
    }
    cam_ = nullptr;
}

void ChaseFovWidener::widen(void* cam, float degrees) {
    if (degrees == game_fov) {
        return;
    }
    game_ = load<float>(cam, chase_cam_fov);
    widened_ = widened_fov(game_, degrees);
    store<float>(cam, chase_cam_fov, widened_);
    cam_ = cam;
}

ChaseFovWidener& chase_fov_widener() {
    static ChaseFovWidener widener;
    return widener;
}

}  // namespace bb::game
