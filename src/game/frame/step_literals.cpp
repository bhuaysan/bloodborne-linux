// Per-frame updates that the game calls with a literal 1/30 s, patched to take the target frame
// rate's step (docs/design/frame-rate.md, "Fades, effects and menus (#556)").
#include "game/frame/step_literals.hpp"

#include "game/frame/frame.hpp"
#include "hook/hook.hpp"

// Filled by hook::apply (BB_PATCH) before it writes the patches.
extern "C" {
std::uint64_t bb_sfx_step_continue = 0;
std::uint64_t bb_shop_tab_continue = 0;
std::uint64_t bb_chr_fade_continue = 0;
std::uint64_t bb_light_step_continue = 0;
std::uint64_t bb_light_step_target = 0;
std::uint64_t bb_light_step_empty_continue = 0;
std::uint64_t bb_hold_continue = 0;
std::uint32_t bb_hold_phase = 0;
}

// Each stub does what the patched bytes do, with bb_frame_step in place of the game's 1/30, and
// changes the same registers and flags: the effects' stub xmm0 and the flags of its cmp; the shop's
// stub xmm0, rdi and what the call changes; the fade's stub xmm0; the lights' stubs xmm4 or xmm0
// and the flags of their cmp (the game's next instructions don't read them); the hold's stub eax
// and the flags, which the game sets again before it reads them (a xor and a call follow). The
// shop's call returns into the stub, with the stack as the game's call has it but for the return
// address.
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

    .globl bb_light_step_stub
    .type bb_light_step_stub, @function
bb_light_step_stub:
    cmp dword ptr [rax + 0x100], 1
    vmovss xmm4, dword ptr [rip + bb_frame_step]
    jne 1f
    vmulss xmm4, xmm4, dword ptr [rip + .Lbb_light_half]
1:
    jmp qword ptr [rip + bb_light_step_target]
    .size bb_light_step_stub, . - bb_light_step_stub

    .globl bb_light_step_empty_stub
    .type bb_light_step_empty_stub, @function
bb_light_step_empty_stub:
    cmp dword ptr [rax + 0x100], 1
    vmovss xmm0, dword ptr [rip + bb_frame_step]
    jne 1f
    vmulss xmm0, xmm0, dword ptr [rip + .Lbb_light_half]
1:
    jmp qword ptr [rip + bb_light_step_empty_continue]
    .p2align 2
.Lbb_light_half:
    .float 0.5
    .size bb_light_step_empty_stub, . - bb_light_step_empty_stub

    .globl bb_hold_count_stub
    .type bb_hold_count_stub, @function
bb_hold_count_stub:
    cmp byte ptr [r12 + 0x4d], 0
    je 4f
    cmp dword ptr [r12 + 0x50], 0
    je 1f
    mov eax, dword ptr [rip + bb_hold_phase]
    add eax, 30
    cmp eax, dword ptr [rip + bb_frame_rate]
    jb 3f
    sub eax, dword ptr [rip + bb_frame_rate]
    dec dword ptr [r12 + 0x50]
3:
    mov dword ptr [rip + bb_hold_phase], eax
    jmp 2f
1:
    mov byte ptr [r12 + 0x4d], 0
4:
    mov dword ptr [rip + bb_hold_phase], 0
2:
    jmp qword ptr [rip + bb_hold_continue]
    .size bb_hold_count_stub, . - bb_hold_count_stub

    .att_syntax prefix
    .popsection
)");

BB_PATCH(bb::game::sfx_frame_update, bb::game::sfx_step_load, bb_sfx_step_stub,
         bb::game::sfx_step_patched, bb_sfx_step_continue);
BB_PATCH(bb::game::shop_tab_refresh, bb::game::shop_tab_update, bb_shop_tab_update_stub,
         bb::game::shop_tab_update_patched, bb_shop_tab_continue);
BB_PATCH(bb::game::chr_list_fade_step, bb::game::chr_fade_add, bb_chr_fade_add_stub,
         bb::game::chr_fade_add_patched, bb_chr_fade_continue);

namespace {
const bb::hook::JumpTarget light_step_targets[] = {
    {bb::game::light_step_target, &bb_light_step_target}};
}  // namespace

BB_PATCH_TARGETS(bb::game::effect_light_gather, bb::game::light_step_load, bb_light_step_stub,
                 bb::game::light_step_patched, bb_light_step_continue, light_step_targets);
BB_PATCH(bb::game::effect_light_gather, bb::game::light_step_empty_load,
         bb_light_step_empty_stub, bb::game::light_step_empty_patched,
         bb_light_step_empty_continue);
BB_PATCH(bb::game::effect_object_update, bb::game::hold_count, bb_hold_count_stub,
         bb::game::hold_count_patched, bb_hold_continue);
