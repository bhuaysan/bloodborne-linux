// The player's chase camera at the target frame rate (docs/design/frame-rate.md, "The chase
// camera").
#pragma once

#include <array>
#include <cstdint>

namespace bb::game {

// The game's chase camera update, 0x0143ac60: `cam` is the camera (built by chase_cam_construct
// 0x0143a0e0), `chr` the player context, `world` the collision world, `dt` the step in seconds.
using ChaseCamUpdate = void(void* cam, void* chr, void* world, float dt);

// Its first 20 bytes: push rbp; mov rbp, rsp; push r15 to rbx; sub rsp, 0x3d8.
inline constexpr std::array<std::uint8_t, 20> chase_cam_prologue = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
    0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0xd8, 0x03, 0x00, 0x00};

// Byte offsets of the camera's fields that work per call, not per second. A factor k blends a
// value toward its target by k per call; a kept fraction keeps that much of the distance per
// call; a stick scale is the turn per call and stick unit.
namespace chase_cam {
// The weight that pulls the lag factors toward 1: set to 1 by camera input, then held for
// +0x134 seconds and dropped by one per +0x1e4 seconds. The game decays it before its use.
inline constexpr std::uint32_t input_weight = 0x130;
inline constexpr std::uint32_t input_hold = 0x134;
inline constexpr std::uint32_t input_fade_time = 0x1e4;
inline constexpr std::uint32_t follow_rate = 0x198;  // follow point, eased toward +0x19c
inline constexpr std::uint32_t follow_rate_target = 0x19c;
inline constexpr std::uint32_t follow_rate_reset = 0x1a0;
inline constexpr std::uint32_t pitch_keep = 0x1a8;  // kept fraction of the pitch pull-back
// Horizontal position lag, eased toward +0x1b0. Unlike follow_rate it is read after its ease.
inline constexpr std::uint32_t lag_rate = 0x1ac;
inline constexpr std::uint32_t lag_rate_target = 0x1b0;
inline constexpr std::uint32_t lag_rate_reset = 0x1b4;  // used in the call of a reset
inline constexpr std::uint32_t stick_x_scale = 0x280;  // lock-on stick turn
inline constexpr std::uint32_t stick_y_scale = 0x284;
// The other factors: position lag (+0x1b8, lock-on +0x1c8, +0x1cc), distance (+0x1c4), stick
// pitch (+0x23c), self-turn (+0x240), lock-on stick smoothing (+0x288), parameters (+0x330).
inline constexpr std::array<std::uint32_t, 10> factors = {
    follow_rate, lag_rate, 0x1b8, 0x1c4, 0x1c8, 0x1cc, 0x23c, 0x240, 0x288, 0x330};
// The game eases follow_rate and lag_rate toward their targets by this factor a call.
inline constexpr float rate_ease = 1.0f / 30.0f;
}  // namespace chase_cam

// Runs `original` once on `cam` as if it ran `calls` times as often: every per-call factor k
// becomes 1 - (1 - k)^(1 / calls), every kept fraction f becomes f^(1 / calls) and every stick
// scale is divided by `calls`, for this call only. Fields the game changes inside the call
// before it uses them (the eased factors, input_weight) are set so that the value the game
// uses is the scaled one, and afterwards get the value the game would give them at its own
// rate over 1 / calls of a call; a path that leaves them alone gets their own value back.
// `eases` tells whether the call will ease them: the game skips the ease when its camera
// collision byte (0x05127a94) is 0. With calls == 1 it only calls `original`. Factors outside
// [0, 1] stay as they are.
void chase_cam_update_scaled(ChaseCamUpdate* original, double calls, bool eases, void* cam,
                             void* chr, void* world, float dt);

// 0x0143ac60, wrapped: runs the original with its per-call factors scaled from 30 calls a second
// to frame_rate() (chase_cam_update_scaled with frame_rate() / 30). At 30 FPS it is the original.
void chase_cam_update(void* cam, void* chr, void* world, float dt);

}  // namespace bb::game
