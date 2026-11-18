// The world's character update intervals at the target frame rate (docs/design/frame-rate.md,
// "The character update interval (#573)"): the game's due check counts an interval in frames;
// at 60 FPS the stub counts it in 30 FPS frames.
#include "game/chr/update_interval.hpp"

#include "game/frame/frame.hpp"
#include "game/image.hpp"
#include "hook/hook.hpp"

// Read by the stub. The scale is set on the main thread by world_chr_update before the check
// runs, on the same thread, in the same call; hook::apply fills the jump targets (BB_PATCH).
extern "C" {
std::int32_t bb_chr_due_scale = 1;
std::uint64_t bb_chr_due_continue = 0;
std::uint64_t bb_chr_due_not_due = 0;
}

// Entered by the patch's jump at 0x01917909 with the game's registers and frame, ecx = u > 0.
// It does what the game's bytes up to 0x0191792c do, with k * u in place of u in the two
// divisions and the conversion, and keeps u in ecx and [rbp-0xc8]. It changes eax, edx, esi and,
// when due, xmm0, as the game's bytes do; the code at both continuations writes eax and esi before
// it reads them, and edx only after a call, at 0x019178ee. rcx is pushed for the divisions and
// popped before either jump. k is 1 or 2 (set_frame_rate allows 30 and 60) and the game's
// intervals are at most 30, so k * u does not overflow.
asm(R"(
    .pushsection .text
    .globl bb_chr_due_check_stub
    .type bb_chr_due_check_stub, @function
    .intel_syntax noprefix
bb_chr_due_check_stub:
    push rcx
    imul ecx, dword ptr [rip + bb_chr_due_scale]
    mov eax, r13d
    cdq
    idiv ecx
    mov esi, edx
    mov eax, dword ptr [rdi + 0xa68]
    xor edx, edx
    div ecx
    cmp esi, edx
    jne 1f
    vcvtsi2ss xmm0, xmm0, ecx
    pop rcx
    mov dword ptr [rbp - 0xc8], ecx
    jmp qword ptr [rip + bb_chr_due_continue]
1:
    pop rcx
    jmp qword ptr [rip + bb_chr_due_not_due]
    .att_syntax prefix
    .size bb_chr_due_check_stub, . - bb_chr_due_check_stub
    .popsection
)");

namespace bb::game {

namespace {

// The due check's other way out, besides its continuation.
constexpr hook::JumpTarget chr_due_targets[] = {{chr_not_due_continue, &bb_chr_due_not_due}};

}  // namespace

void set_chr_due_scale(std::int32_t scale) {
    bb_chr_due_scale = scale;
}

void world_chr_update_with(WorldChrUpdate* original, float step, void* world) {
    set_chr_due_scale(static_cast<std::int32_t>(frame_rate_scale()));
    original(step, world);
}

// 0x019173b0
void world_chr_update(float step, void* world) {
    world_chr_update_with(reinterpret_cast<WorldChrUpdate*>(hook::original(image(), 0x019173b0)),
                          step, world);
}

}  // namespace bb::game

BB_WRAP(0x019173b0, bb::game::world_chr_update, bb::game::world_chr_update_prologue);
BB_PATCH_TARGETS(0x019173b0, bb::game::chr_due_check, bb_chr_due_check_stub,
                 bb::game::chr_due_check_patched, bb_chr_due_continue, bb::game::chr_due_targets);
