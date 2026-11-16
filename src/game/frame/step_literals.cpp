// Per-frame updates that the game calls with a literal 1/30 s, patched to take the target frame
// rate's step (docs/design/frame-rate.md, "Fades, effects and menus (#556)").
#include "game/frame/step_literals.hpp"

#include "game/frame/frame.hpp"
#include "game/image.hpp"
#include "hook/hook.hpp"

// Written once by set_step_literal_targets, before the patches run.
extern "C" {
std::uint64_t bb_sfx_step_continue = 0;
std::uint64_t bb_shop_tab_continue = 0;
std::uint64_t bb_chr_fade_continue = 0;
}

// Each stub does what the patched bytes do, with bb_frame_step in place of the game's 1/30, and
// changes the same registers and flags: the effects' stub xmm0 and the flags of its cmp; the shop's
// stub xmm0, rdi and what the call changes; the fade's stub xmm0. The shop's call returns into the
// stub, with the stack as the game's call has it but for the return address.
asm(R"(
    .pushsection .text
    .intel_syntax noprefix

    .globl bb_sfx_step_stub
    .type bb_sfx_step_stub, @function
bb_sfx_step_stub:
    cmp dword ptr [r12 + 0x8c], 0
    vxorps xmm0, xmm0, xmm0
    jne 1f
    vmovss xmm0, dword ptr [rip + bb_frame_step]
1:
    jmp qword ptr [rip + bb_sfx_step_continue]
    .size bb_sfx_step_stub, . - bb_sfx_step_stub

    .globl bb_shop_tab_update_stub
    .type bb_shop_tab_update_stub, @function
bb_shop_tab_update_stub:
    vmovss xmm0, dword ptr [rip + bb_frame_step]
    mov rdi, rax
    call qword ptr [rcx + 0x88]
    jmp qword ptr [rip + bb_shop_tab_continue]
    .size bb_shop_tab_update_stub, . - bb_shop_tab_update_stub

    .globl bb_chr_fade_add_stub
    .type bb_chr_fade_add_stub, @function
bb_chr_fade_add_stub:
    vaddss xmm0, xmm0, dword ptr [rip + bb_frame_step]
    vminss xmm0, xmm0, dword ptr [rip + .Lbb_fade_one]
    jmp qword ptr [rip + bb_chr_fade_continue]
    .p2align 2
.Lbb_fade_one:
    .float 1.0
    .size bb_chr_fade_add_stub, . - bb_chr_fade_add_stub

    .att_syntax prefix
    .popsection
)");

namespace bb::game {

void set_step_literal_targets() {
    bb_sfx_step_continue = image().runtime(sfx_step_continue);
    bb_shop_tab_continue = image().runtime(shop_tab_continue);
    bb_chr_fade_continue = image().runtime(chr_fade_continue);
}

}  // namespace bb::game

BB_PATCH(bb::game::sfx_frame_update, bb::game::sfx_step_load, bb_sfx_step_stub,
         bb::game::sfx_step_patched);
BB_PATCH(bb::game::shop_tab_refresh, bb::game::shop_tab_update, bb_shop_tab_update_stub,
         bb::game::shop_tab_update_patched);
BB_PATCH(bb::game::chr_list_fade_step, bb::game::chr_fade_add, bb_chr_fade_add_stub,
         bb::game::chr_fade_add_patched);
