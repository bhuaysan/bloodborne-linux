// The loading page's minimum time on an area load (#684): the hold MoveMapStep sets, taken from
// bb_load_hold in place of the game's 12 s literal.
#include "game/load/load_hold.hpp"

#include <format>
#include <stdexcept>

#include "hook/hook.hpp"

extern "C" {
float bb_load_hold = bb::game::game_load_hold;
std::uint64_t bb_load_hold_continue = 0;  // filled by hook::apply (BB_PATCH)
}

// Does what the patched bytes do with bb_load_hold in place of [0x049280c8], and changes the same
// registers and flags: xmm0 (all lanes but the first 0, as the game's vmovss and vxorps leave them)
// and the flags of the cmp.
asm(R"(
    .pushsection .text
    .intel_syntax noprefix
    .globl bb_load_hold_stub
    .type bb_load_hold_stub, @function
bb_load_hold_stub:
    cmp byte ptr [r12 + 0xe4], 0
    vxorps xmm0, xmm0, xmm0
    je 1f
    vmovss xmm0, dword ptr [rip + bb_load_hold]
1:
    jmp qword ptr [rip + bb_load_hold_continue]
    .size bb_load_hold_stub, . - bb_load_hold_stub
    .att_syntax prefix
    .popsection
)");

BB_PATCH(bb::game::move_map_load_start, bb::game::load_hold_load, bb_load_hold_stub,
         bb::game::load_hold_patched, bb_load_hold_continue);

namespace bb::game {

void set_load_hold(float seconds) {
    if (!(seconds >= 0.0f && seconds <= game_load_hold)) {
        throw std::invalid_argument(
            std::format("load hold {} is not from 0 to {} seconds", seconds, game_load_hold));
    }
    bb_load_hold = seconds;
}

float load_hold() { return bb_load_hold; }

}  // namespace bb::game
