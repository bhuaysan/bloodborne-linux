// The characters' ragdolls at the target frame rate (docs/design/frame-rate.md, "The ragdolls
// (#567)"): the per-frame ragdoll update gets the frame's step instead of a literal 1/30, and the
// two per-frame impulses inside it keep what they add up to in a game second.
#pragma once

#include <array>
#include <cstdint>

namespace bb::game {

// ---- the pose sync -----------------------------------------------------------------------------

// 0x01515ec0, a character's physics pose sync, called once a frame for every character (from the
// physics module's frame call 0x01515cc0 and a character virtual, 0x018bfe50), whatever its update
// interval. `module` is the character's physics module (chr+0x58): +0x08 the character, +0x10 an
// object whose +0x20 is the fallback lock object, +0x20 the ragdoll. `transform` is the module's
// +0x90, the character's 3x4 world transform. It picks the lock object (the character's +0x3b0
// +0x30 +0x28 when that object's +0x384 is set), runs its one-time init under it (bit 0 of +0xc,
// +0x10 swapped from 0 to 1 by the first caller, then 0x01d3c500), and hands the ragdoll, the
// transform and the lock object's +0x38 to one of three ragdoll updates:
//  - the character updates every frame (interval chr+0xe4 == 0) and is active:
//    ragdoll_update (0x0180df90) with the step, 30.0 (not read: it clears xmm1 first) and the flag
//    "+0x3b0 +0x68 +0x2f4 below 1", after setting the ragdoll's +0x150 to the character's +0x3b0
//    +0x68 +0x1e0 raised by +0x354 in y and its +0x160 to 1;
//  - it updates every u > 0 frames: ragdoll_update_spaced (0x01811040) with the step, u / 30 s and
//    the flag "bit 2 of chr+0x1e1 clear";
//  - otherwise (not active, or a negative interval): ragdoll_update_idle (0x01811650) with the
//    step and 900.0 (not read: it overwrites xmm1 at 0x0181171d).
// Active: the character's +0x18 has bit 0 of +0x20 clear, bit 1 clear or its +0x30 set with bit 0
// of +0x18, and +0x10 == 4. TODO(name): what +0x18 and its state 4 are.
// The game passes a literal 1/30 as the step; this passes frame_step(). u / 30 s stays: since #573
// a character with interval u updates every u 30 FPS frames at any frame rate, and the update
// waits for that much accumulated step before it drives the ragdoll.
void chr_physics_pose_sync(void* module, void* transform);

inline constexpr std::uint64_t chr_physics_pose_sync_link = 0x01515ec0;
inline constexpr std::uint64_t ragdoll_update = 0x0180df90;
inline constexpr std::uint64_t ragdoll_update_spaced = 0x01811040;
inline constexpr std::uint64_t ragdoll_update_idle = 0x01811650;
inline constexpr std::uint64_t ragdoll_lock_init = 0x01d3c500;  // TODO(name)

// ---- the impulses inside ragdoll_update (0x0180df90) -------------------------------------------

// The timed radial impulse. While the ragdoll's timer (+0xf0) is above 0 the update lowers it by
// the step and applies an impulse to each body (0x00b68db0, hknp's linear impulse), away from the
// centre (+0xe0) with a strength between +0xfc and +0x100 by the distance between +0xf4 and +0xf8.
// Once a frame, so with the step at 1/60 it would add up to twice the impulse in the timer's
// time. The patch scales both strengths by bb_step_ratio where the update loads them:
//   vmovss xmm2, [r12+0xfc]; vmovss [rbp-0x188], xmm2
//   vmovss xmm3, [r12+0x100]; vmovss [rbp-0x1d0], xmm3
// No jump of the game lands inside these bytes; the stub jumps back to the instruction behind
// them, at 0x0180e0d3.
inline constexpr std::uint64_t ragdoll_impulse_load = 0x0180e0af;
inline constexpr std::uint64_t ragdoll_impulse_continue = 0x0180e0d3;
inline constexpr std::array<std::uint8_t, 36> ragdoll_impulse_patched = {
    0xc4, 0xc1, 0x7a, 0x10, 0x94, 0x24, 0xfc, 0x00, 0x00, 0x00, 0xc5, 0xfa,
    0x11, 0x95, 0x78, 0xfe, 0xff, 0xff, 0xc4, 0xc1, 0x7a, 0x10, 0x9c, 0x24,
    0x00, 0x01, 0x00, 0x00, 0xc5, 0xfa, 0x11, 0x9d, 0x30, 0xfe, 0xff, 0xff};

// The anti-penetration pull of the root body ("Enable Penetrate", the debug switch at 0x051282d0,
// on by default): when the root penetrates, the update adds (target +0x1a0 - root position) * 30
// to its velocity, as an impulse of that times its mass, every frame. That is a spring of 900/s²
// at 30 FPS; once a frame at 60 it would be twice as stiff. The patch makes the factor
// 30 * bb_step_ratio (15 at 60):
//   vmulss xmm1, xmm1, [0x049275e8] (30.0); vpshufd xmm1, xmm1, 0; vmulps xmm0, xmm0, xmm1
// No jump of the game lands inside these bytes; the stub jumps back to the instruction behind
// them, at 0x0180f16f.
inline constexpr std::uint64_t ragdoll_root_pull = 0x0180f15e;
inline constexpr std::uint64_t ragdoll_root_pull_continue = 0x0180f16f;
inline constexpr std::array<std::uint8_t, 17> ragdoll_root_pull_patched = {
    0xc5, 0xf2, 0x59, 0x0d, 0x82, 0x84, 0x11, 0x03, 0xc5,
    0xf9, 0x70, 0xc9, 0x00, 0xc5, 0xf8, 0x59, 0xc1};

// hook::apply jumps each stub back behind its patched bytes; these are those places.
static_assert(ragdoll_impulse_continue == ragdoll_impulse_load + ragdoll_impulse_patched.size());
static_assert(ragdoll_root_pull_continue == ragdoll_root_pull + ragdoll_root_pull_patched.size());

}  // namespace bb::game

// The stubs (BB_PATCH), written in assembly for their places, and their jump targets, which
// hook::apply fills.
extern "C" {
void bb_ragdoll_impulse_stub();
void bb_ragdoll_root_pull_stub();
extern std::uint64_t bb_ragdoll_impulse_continue;
extern std::uint64_t bb_ragdoll_root_pull_continue;
}
