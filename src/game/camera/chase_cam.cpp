// The player's chase camera at the target frame rate (docs/design/frame-rate.md, "The chase
// camera"): a wrapper around the game's update that scales its per-call filters.
#include "game/camera/chase_cam.hpp"

#include <cmath>
#include <cstddef>
#include <cstring>

#include "game/frame/frame.hpp"
#include "game/image.hpp"
#include "hook/hook.hpp"

namespace bb::game {
namespace {

float load(const void* cam, std::uint32_t offset) {
    float value;
    std::memcpy(&value, static_cast<const std::byte*>(cam) + offset, sizeof value);
    return value;
}

void store(void* cam, std::uint32_t offset, float value) {
    std::memcpy(static_cast<std::byte*>(cam) + offset, &value, sizeof value);
}

bool is_fraction(float k) {
    return k >= 0.0f && k <= 1.0f;
}

// k per call, as a factor per 1/calls of a call.
float scale_factor(float k, double calls) {
    return is_fraction(k) ? static_cast<float>(1.0 - std::pow(1.0 - k, 1.0 / calls)) : k;
}

float scale_kept(float f, double calls) {
    return is_fraction(f) ? static_cast<float>(std::pow(f, 1.0 / calls)) : f;
}

// A field set to `scaled` for the call, and its own value.
struct Scaled {
    std::uint32_t offset;
    float own, scaled;
};

// The game's ease of an eased factor: `rate` toward `target` by rate_ease a call, or `reset`
// when the camera resets it. Redone from the value before the call over 1/calls of a call.
void ease_again(void* cam, const Scaled& rate, std::uint32_t target, std::uint32_t reset,
                double calls) {
    const float now = load(cam, rate.offset);
    if (now == load(cam, reset)) {
        return;  // reset during the call
    }
    const float k = scale_factor(chase_cam::rate_ease, calls);
    store(cam, rate.offset, rate.own + (load(cam, target) - rate.own) * k);
}

}  // namespace

void chase_cam_update_scaled(ChaseCamUpdate* original, double calls, void* cam, void* chr,
                             void* world, float dt) {
    if (calls == 1.0) {
        original(cam, chr, world, dt);
        return;
    }
    using namespace chase_cam;
    Scaled fields[factors.size() + 3];
    std::size_t n = 0;
    for (std::uint32_t offset : factors) {
        const float own = load(cam, offset);
        fields[n++] = {offset, own, scale_factor(own, calls)};
    }
    const float keep = load(cam, pitch_keep);
    fields[n++] = {pitch_keep, keep, scale_kept(keep, calls)};
    for (std::uint32_t offset : {stick_x_scale, stick_y_scale}) {
        const float own = load(cam, offset);
        fields[n++] = {offset, own, static_cast<float>(own / calls)};
    }
    for (const Scaled& f : fields) {
        store(cam, f.offset, f.scaled);
    }

    original(cam, chr, world, dt);

    for (const Scaled& f : fields) {
        if (f.offset == follow_rate) {
            ease_again(cam, f, follow_rate_target, follow_rate_reset, calls);
        } else if (f.offset == lag_rate) {
            ease_again(cam, f, lag_rate_target, lag_rate_reset, calls);
        } else if (load(cam, f.offset) == f.scaled) {
            store(cam, f.offset, f.own);  // a field the call wrote keeps the new value
        }
    }
}

// 0x0143ac60
void chase_cam_update(void* cam, void* chr, void* world, float dt) {
    auto* const original =
        reinterpret_cast<ChaseCamUpdate*>(hook::original(image(), 0x0143ac60));
    chase_cam_update_scaled(original, frame_rate() / 30.0, cam, chr, world, dt);
}

}  // namespace bb::game

BB_WRAP(0x0143ac60, bb::game::chase_cam_update, bb::game::chase_cam_prologue);
