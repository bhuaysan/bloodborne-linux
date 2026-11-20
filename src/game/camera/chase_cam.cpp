// The player's chase camera at the target frame rate (docs/design/frame-rate.md, "The chase
// camera"): a wrapper around the game's update that scales its per-call filters.
#include "game/camera/chase_cam.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "game/camera/field_of_view.hpp"
#include "game/frame/frame.hpp"
#include "game/image.hpp"
#include "hook/hook.hpp"

namespace bb::game {
namespace {

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

// An eased factor (follow_rate, lag_rate): the game sets it to `reset` or moves it toward
// `target` by rate_ease a call, or leaves it alone.
struct Eased {
    std::uint32_t offset, reset;
    float own, eased;              // eased: over 1 / calls of a call
    float set;                     // the value before the call
    float own_reset, reset_set;    // the reset value, and the one before the call

    void prepare(void* cam) const {
        store<float>(cam, offset, set);
        store<float>(cam, reset, reset_set);
    }

    // After the call: a reset gives the reset value, a skipped ease the own value, an ease the
    // eased one.
    void settle(void* cam) const {
        const float now = load<float>(cam, offset);
        store<float>(cam, reset, own_reset);
        store<float>(cam, offset, now == reset_set ? own_reset : now == set ? own : eased);
    }
};

// An eased factor read before its ease: the game uses the value it had before the call.
Eased eased_factor(const void* cam, std::uint32_t offset, std::uint32_t target,
                   std::uint32_t reset, double calls) {
    const float own = load<float>(cam, offset);
    const float eased =
        own + (load<float>(cam, target) - own) * scale_factor(chase_cam::rate_ease, calls);
    const float own_reset = load<float>(cam, reset);
    return {offset, reset, own, eased, scale_factor(own, calls), own_reset, own_reset};
}

}  // namespace

void chase_cam_update_scaled(ChaseCamUpdate* original, double calls, bool eases, void* cam,
                             void* chr, void* world, float dt) {
    if (calls == 1.0) {
        original(cam, chr, world, dt);
        return;
    }
    using namespace chase_cam;

    // Fields the game only reads: scaled for the call, their own value afterwards.
    Scaled plain[factors.size() - 2 + 3];  // without the eased two; pitch_keep, stick scales
    std::size_t n = 0;
    for (std::uint32_t offset : factors) {
        if (offset != follow_rate && offset != lag_rate) {
            const float own = load<float>(cam, offset);
            plain[n++] = {offset, own, scale_factor(own, calls)};
        }
    }
    const float keep = load<float>(cam, pitch_keep);
    plain[n++] = {pitch_keep, keep, scale_kept(keep, calls)};
    for (std::uint32_t offset : {stick_x_scale, stick_y_scale}) {
        const float own = load<float>(cam, offset);
        plain[n++] = {offset, own, static_cast<float>(own / calls)};
    }

    // follow_rate is read before its ease, so it is set to the scaled value. lag_rate is read
    // after it: set to what eases into the scaled value of its eased value when the ease runs,
    // and its reset value is scaled, since a reset call uses it too.
    const Eased follow =
        eased_factor(cam, follow_rate, follow_rate_target, follow_rate_reset, calls);
    Eased lag = eased_factor(cam, lag_rate, lag_rate_target, lag_rate_reset, calls);
    if (eases) {
        const float lag_used = scale_factor(lag.eased, calls);
        lag.set = (lag_used - load<float>(cam, lag_rate_target) * rate_ease) / (1.0f - rate_ease);
    }
    lag.reset_set = scale_factor(lag.own_reset, calls);

    // input_weight: the game drops it by `fade` once the hold has run out (as it computes
    // that), then uses it. It is set so that the dropped value is the scaled one.
    const float own_weight = load<float>(cam, input_weight);
    const float fade = dt / load<float>(cam, input_fade_time);
    const bool fades = !(0.0f < std::max(0.0f, load<float>(cam, input_hold) - dt));
    const float weight = fades ? std::max(0.0f, own_weight - fade) : own_weight;
    const float weight_used = scale_factor(weight, calls);
    const float weight_set = fades ? weight_used + fade : weight_used;

    for (std::size_t i = 0; i < n; ++i) {
        store<float>(cam, plain[i].offset, plain[i].scaled);
    }
    follow.prepare(cam);
    lag.prepare(cam);
    store<float>(cam, input_weight, weight_set);

    original(cam, chr, world, dt);

    for (std::size_t i = 0; i < n; ++i) {
        store<float>(cam, plain[i].offset, plain[i].own);
    }
    follow.settle(cam);
    lag.settle(cam);
    // Camera input sets the weight to 1; a call that didn't drop it gives the own value back.
    const float now_weight = load<float>(cam, input_weight);
    if (now_weight != 1.0f) {
        store<float>(cam, input_weight, now_weight == weight_set ? own_weight : weight);
    }
}

// 0x0143ac60
void chase_cam_update(void* cam, void* chr, void* world, float dt) {
    auto* const original =
        reinterpret_cast<ChaseCamUpdate*>(hook::original(image(), 0x0143ac60));
    const bool eases = global<std::uint8_t>(0x05127a94) != 0;  // camera collision on
    ChaseFovWidener& fov = chase_fov_widener();
    fov.restore(cam);
    chase_cam_update_scaled(original, static_cast<double>(frame_rate()) / default_frame_rate,
                            eases, cam, chr, world, dt);
    fov.widen(cam, field_of_view());
}

}  // namespace bb::game

BB_WRAP(0x0143ac60, bb::game::chase_cam_update, bb::game::chase_cam_prologue);
