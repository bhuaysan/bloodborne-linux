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

// Points the stubs at the image's code behind their patches. Called once before the hooks run
// game code (bb-run's start, and the tests).
void set_step_literal_targets();

}  // namespace bb::game

// The stubs (BB_PATCH), written in assembly for their places, and what they read besides
// bb_frame_step: the absolute addresses to jump back to.
extern "C" {
void bb_sfx_step_stub();
void bb_shop_tab_update_stub();
void bb_chr_fade_add_stub();
extern std::uint64_t bb_sfx_step_continue;
extern std::uint64_t bb_shop_tab_continue;
extern std::uint64_t bb_chr_fade_continue;
}
