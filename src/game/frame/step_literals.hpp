// Per-frame updates that the game calls with a literal 1/30 s, patched to take the target frame
// rate's step (docs/design/frame-rate.md, "Fades, effects and menus (#556)"). Each is a BB_PATCH
// of a few instructions inside the calling function: a stub that does what those bytes do with
// bb_frame_step (or a step derived from it) in place of the literal, then jumps back. At 30 FPS
// the stubs load the same float the game's bytes load (0x3d088889).
#pragma once

#include <array>
#include <cstdint>

namespace bb::game {

// ---- the effects (SprjSfx) ---------------------------------------------------------------------

// SprjSfx's frame update, 0x01f77bd0, called once a frame by SprjGraphicsStep::STEP_Update
// (0x01d9b340) with the singleton at 0x05540370. At its end it steps the effect system (SprjSfx
// +0x70, built at 0x01f77250; its virtual +0x40 is 0x022ff340, which takes the step in xmm0):
//   cmp dword [r12+0x8c], 0; vxorps xmm0, xmm0, xmm0; jne 0x01f77d52     (a skip flag: step 0)
//   vmovss xmm0, [0x04929170]                                             (else 1/30)
// and calls it at 0x01f77d52 (call rax), which nothing else in these bytes jumps to.
inline constexpr std::uint64_t sfx_frame_update = 0x01f77bd0;
inline constexpr std::uint64_t sfx_step_load = 0x01f77d3b;
inline constexpr std::uint64_t sfx_step_continue = 0x01f77d52;
inline constexpr std::array<std::uint8_t, 23> sfx_step_patched = {
    0x41, 0x83, 0xbc, 0x24, 0x8c, 0x00, 0x00, 0x00, 0x00, 0xc5, 0xf8, 0x57,
    0xc0, 0x75, 0x08, 0xc5, 0xfa, 0x10, 0x05, 0x1e, 0x14, 0x9b, 0x02};

// ---- the shop's tab highlight ------------------------------------------------------------------

// 0x01734510, the shop menu's (S02) tab refresh: finds the layout instance "Ins_tab_select" (or
// "Ins_change_tab_select") and updates it with 1/30 through its virtual +0x88, the layout
// instances' update (0x00cec810 for a plain one, which advances its animations and its children by
// the step). The shop's frame update (0x01736140) calls it every frame after its own update has
// advanced the whole layout by the frame's step, so the highlight gets one extra update of 1/30 a
// frame. 0x01732fe0 (after setting +0x1e1) and the message handler 0x01733fa0 (message 0x9c9)
// call it once on an event; the patch gives those calls the frame's step as well. At 0x0173456e,
// with rax = the instance and rcx = its vtable:
//   vmovss xmm0, [0x049270e4]; mov rdi, rax; call [rcx+0x88]
// No jump of the game lands inside these bytes; the one behind them is at 0x0173457f.
inline constexpr std::uint64_t shop_tab_refresh = 0x01734510;
inline constexpr std::uint64_t shop_tab_update = 0x0173456e;
inline constexpr std::uint64_t shop_tab_continue = 0x0173457f;
inline constexpr std::array<std::uint8_t, 17> shop_tab_update_patched = {
    0xc5, 0xfa, 0x10, 0x05, 0x6e, 0x2b, 0x1f, 0x03, 0x48,
    0x89, 0xc7, 0xff, 0x91, 0x88, 0x00, 0x00, 0x00};

// ---- the character fade ------------------------------------------------------------------------

// 0x0180f5e0, called from a character's physics pose sync (0x01515ec0, through 0x0180df90 and
// 0x01811040): it raises a fade at +0x1c0 by 1/30 a call up to 1 and copies it to the list's
// parts of kind 7. At 0x0180f647, with xmm0 = the fade, below 1:
//   vaddss xmm0, xmm0, [0x049275f4] (1/30); vminss xmm0, xmm0, [0x049275f0] (1.0)
// No jump lands inside these bytes; the one behind them is at 0x0180f657.
inline constexpr std::uint64_t chr_list_fade_step = 0x0180f5e0;
inline constexpr std::uint64_t chr_fade_add = 0x0180f647;
inline constexpr std::uint64_t chr_fade_continue = 0x0180f657;
inline constexpr std::array<std::uint8_t, 16> chr_fade_add_patched = {
    0xc5, 0xfa, 0x58, 0x05, 0xa5, 0x7f, 0x11, 0x03,
    0xc5, 0xfa, 0x5d, 0x05, 0x99, 0x7f, 0x11, 0x03};

// ---- the effect lights -------------------------------------------------------------------------

// 0x02313870 gathers the dynamic lights of the effects attached to a model into the model's light
// list (two banks of four; called once a frame per model by 0x01d6f9d0 and 0x01d24660, with the
// model's effect handle into the table of the effect object at 0x05540e00). Its step fades a light
// in over 0.1 s and out over 0.75 s, ages it and scales an offset. The step is not the frame's:
// it is 1/60 when the graphics object's present_interval ([0x055406c8]+0x100) is 1 and 1/30
// otherwise, per call. The game's default flip mode 4 has interval 1, so the lights take 1/60 a
// 30 FPS frame. Both loads of the step are patched to that literal times bb_step_ratio:
// bb_frame_step / 2 for interval 1, bb_frame_step otherwise (at 30 the same floats).
// The main path, at 0x02313d80 with rax = the graphics object, continuing at 0x02315d79:
//   cmp dword [rax+0x100], 1; jne 0x02315d71 (vmovss xmm4, [0x04929e74] (1/30))
//   vmovss xmm4, [0x04929e70] (1/60); jmp 0x02315d79
// Nothing else jumps into these bytes; 0x02315d71 is reached only from the jne.
inline constexpr std::uint64_t effect_light_gather = 0x02313870;
inline constexpr std::uint64_t light_step_load = 0x02313d80;
inline constexpr std::uint64_t light_step_continue = 0x02313d9a;
inline constexpr std::uint64_t light_step_target = 0x02315d79;
inline constexpr std::array<std::uint8_t, 26> light_step_patched = {
    0x83, 0xb8, 0x00, 0x01, 0x00, 0x00, 0x01, 0x0f, 0x85, 0xe4, 0x1f, 0x00, 0x00,
    0xc5, 0xfa, 0x10, 0x25, 0xdb, 0x60, 0x61, 0x02, 0xe9, 0xdf, 0x1f, 0x00, 0x00};
// The path for a handle with no table entry, at 0x02313da4 (behind the jump target 0x02313d9a,
// whose lea loads rax), the same choice into xmm0:
//   cmp dword [rax+0x100], 1; jne +10; vmovss xmm0, [0x04929e70]; jmp +8; vmovss xmm0, [0x04929e74]
// Its jumps land inside the bytes or right behind them, at 0x02313dbf.
inline constexpr std::uint64_t light_step_empty_load = 0x02313da4;
inline constexpr std::uint64_t light_step_empty_continue = 0x02313dbf;
inline constexpr std::array<std::uint8_t, 27> light_step_empty_patched = {
    0x83, 0xb8, 0x00, 0x01, 0x00, 0x00, 0x01, 0x75, 0x0a, 0xc5, 0xfa, 0x10, 0x05, 0xbb,
    0x60, 0x61, 0x02, 0xeb, 0x08, 0xc5, 0xfa, 0x10, 0x05, 0xb5, 0x60, 0x61, 0x02};

// ---- the effect object's hold -------------------------------------------------------------------

// 0x023174e0, the frame update of the effect object at 0x05540e00, called once a frame by the
// effect system's update (0x022ff340). While +0x4d is set it counts +0x50 down by one a call and
// clears +0x4d at 0; 0x02313870 gives the lights a step of 0 meanwhile. The pacer's request_0x274
// and SprjScaleform (0x01cf4fda) set +0x50 to 60: two seconds at 30 FPS. The patch counts +0x50
// down on the calls where a 30 Hz clock of game time ticks: a phase in 1/bb_frame_rate of a call
// goes up by 30 a call, and when it reaches bb_frame_rate it drops by that and +0x50 goes down.
// That is every (bb_frame_rate / 30)-th call at a whole multiple of 30, 4 or 5 calls apart at 144,
// so the hold lasts 60 game frames at any rate. The phase is 0 in every call without a running
// hold, so a hold armed from outside starts at 0; one armed again while a hold runs keeps the
// phase, up to one 30 FPS frame short.
// At 0x0231750a, with r12 = the object:
//   cmp byte [r12+0x4d], 0; je 0x0231752a; mov eax, [r12+0x50]; test eax, eax; je 0x02317524
//   dec eax; mov [r12+0x50], eax; jmp 0x0231752a; 0x02317524: mov byte [r12+0x4d], 0
// Nothing else jumps into these bytes; the jumps out land right behind them, at 0x0231752a.
inline constexpr std::uint64_t effect_object_update = 0x023174e0;
inline constexpr std::uint64_t hold_count = 0x0231750a;
inline constexpr std::uint64_t hold_continue = 0x0231752a;
inline constexpr std::array<std::uint8_t, 32> hold_count_patched = {
    0x41, 0x80, 0x7c, 0x24, 0x4d, 0x00, 0x74, 0x18, 0x41, 0x8b, 0x44, 0x24, 0x50, 0x85, 0xc0, 0x74,
    0x09, 0xff, 0xc8, 0x41, 0x89, 0x44, 0x24, 0x50, 0xeb, 0x06, 0x41, 0xc6, 0x44, 0x24, 0x4d, 0x00};

// hook::apply jumps each stub back behind its patched bytes; these are those places.
static_assert(sfx_step_continue == sfx_step_load + sfx_step_patched.size());
static_assert(shop_tab_continue == shop_tab_update + shop_tab_update_patched.size());
static_assert(chr_fade_continue == chr_fade_add + chr_fade_add_patched.size());
static_assert(light_step_continue == light_step_load + light_step_patched.size());
static_assert(light_step_empty_continue == light_step_empty_load + light_step_empty_patched.size());
static_assert(hold_continue == hold_count + hold_count_patched.size());

}  // namespace bb::game

// The stubs (BB_PATCH), written in assembly for their places, and what they read besides
// bb_frame_step: the absolute addresses to jump back to, which hook::apply fills.
extern "C" {
void bb_sfx_step_stub();
void bb_shop_tab_update_stub();
void bb_chr_fade_add_stub();
void bb_light_step_stub();
void bb_light_step_empty_stub();
void bb_hold_count_stub();
extern std::uint64_t bb_sfx_step_continue;
extern std::uint64_t bb_shop_tab_continue;
extern std::uint64_t bb_chr_fade_continue;
extern std::uint64_t bb_light_step_continue;
extern std::uint64_t bb_light_step_target;  // 0x02315d79
extern std::uint64_t bb_light_step_empty_continue;
extern std::uint64_t bb_hold_continue;
// The hold's phase since its last count, 0 to bb_frame_rate - 1 in 1/bb_frame_rate of a call (30
// a call; the effect object is a singleton).
extern std::uint32_t bb_hold_phase;
}
