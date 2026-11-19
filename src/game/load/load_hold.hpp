// The loading page's minimum time on an area load (#684), and bb-run's --load-hold that shortens it.
#pragma once

#include <array>
#include <cstdint>

namespace bb::game {

// MoveMapStep, the step that loads an area and enters it (vtable 0x05330480, built at 0x01937570),
// keeps the loading page up while a hold at +0x278 runs: its frame update (0x019386e0) counts the
// hold down by the frame's step to 0, and the step that ends the load (0x0193a3e0) closes the page
// (0x0176d3c0) only once the hold is 0 and the area is ready. 0x01938870, run when the load starts,
// sets the hold to 12 s ([0x049280c8]) when the load's request has +0xe4 set (copied from the
// request's +0x94 by the constructor) and to 0 otherwise. The death's reload in #655 has it: its
// work is done 4 s into the page, which stays up 13 s. Continue and the lantern travel have 0.
// At 0x01938b59, with r12 = the step:
//   cmp byte [r12+0xe4], 0; je 0x01938b6e; vmovss xmm0, [0x049280c8]; jmp 0x01938b72
//   0x01938b6e: vxorps xmm0, xmm0, xmm0
// and 0x01938b72 stores xmm0 to +0x278. The game jumps to 0x01938b59 from four places (0x01938a24
// to 0x01938a4b), into the bytes only from these jumps of their own.
inline constexpr std::uint64_t move_map_load_start = 0x01938870;
inline constexpr std::uint64_t load_hold_load = 0x01938b59;
inline constexpr std::uint64_t load_hold_continue = 0x01938b72;
inline constexpr std::array<std::uint8_t, 25> load_hold_patched = {
    0x41, 0x80, 0xbc, 0x24, 0xe4, 0x00, 0x00, 0x00, 0x00, 0x74, 0x0a, 0xc5, 0xfa,
    0x10, 0x05, 0x5c, 0xf5, 0xfe, 0x02, 0xeb, 0x04, 0xc5, 0xf8, 0x57, 0xc0};
static_assert(load_hold_continue == load_hold_load + load_hold_patched.size());

// The game's hold, 12 s, and the most --load-hold takes.
inline constexpr float game_load_hold = 12.0f;

// Sets the hold of the loads that have one, from 0 to game_load_hold seconds (anything else throws
// std::invalid_argument). The default is the game's. Called before the game runs; not thread-safe.
void set_load_hold(float seconds);
float load_hold();

}  // namespace bb::game

extern "C" {
// The stub (BB_PATCH) and what it reads: the hold in seconds, and the absolute address to jump back
// to, which hook::apply fills.
void bb_load_hold_stub();
extern float bb_load_hold;
extern std::uint64_t bb_load_hold_continue;
}
