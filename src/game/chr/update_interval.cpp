// The world's character update intervals at the target frame rate (docs/design/frame-rate.md,
// "The character update interval (#573)"): the game's due check counts an interval in frames;
// above 30 FPS the stub counts it in ticks of a 30 Hz clock of game time.
#include "game/chr/update_interval.hpp"

#include "game/frame/frame.hpp"
#include "game/image.hpp"
#include "hook/hook.hpp"

// Read by the stub. They are set on the main thread by world_chr_update before the check runs, on
// the same thread, in the same call; hook::apply fills the jump targets (BB_PATCH).
extern "C" {
std::int32_t bb_chr_due_scale = 1;
std::int32_t bb_chr_due_rate = bb::game::default_frame_rate;
std::uint64_t bb_chr_due_continue = 0;
std::uint64_t bb_chr_due_not_due = 0;
}

// Entered by the patch's jump at 0x01917909 with the game's registers and frame, ecx = u > 0,
// r13d = the index, rdi = WorldChrMan. It keeps u in ecx and [rbp-0xc8] and changes eax, edx, esi
// and, when due, xmm0, as the game's bytes do; the code at both continuations writes eax and esi
// before it reads them, and edx only after a call, at 0x019178ee. rcx is pushed for the divisions
// and popped before either jump.
//
// With bb_chr_due_scale k > 0 (the rate is 30 k) it does what the game's bytes up to 0x0191792c
// do, with k * u in place of u in the two divisions and the conversion. k is at most a few and the
// game's intervals are at most 30, so k * u does not overflow.
//
// With k = 0 (a rate F = bb_chr_due_rate that is not a whole multiple of 30) it computes the phase
//   p = (counter - index) * 30 mod u * F      (counter = [rdi+0xa68]; 64-bit, p in [0, u * F))
// and the character is due when p < 30, with (p + u * F) / 30 frames as its step: the frames
// since the last frame where it was due. (At F = 30 k that would be the check above.) Here it also
// changes the upper halves of rax, rdx and rsi, which the game's 32-bit writes clear.
asm(R"(
    .pushsection .text
    .globl bb_chr_due_check_stub
    .type bb_chr_due_check_stub, @function
    .intel_syntax noprefix
bb_chr_due_check_stub:
    push rcx
    mov eax, dword ptr [rip + bb_chr_due_scale]
    test eax, eax
    jz 3f
    imul ecx, eax
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
0:
    pop rcx
    mov dword ptr [rbp - 0xc8], ecx
    jmp qword ptr [rip + bb_chr_due_continue]
1:
    pop rcx
    jmp qword ptr [rip + bb_chr_due_not_due]
3:
    imul ecx, dword ptr [rip + bb_chr_due_rate]
    mov eax, dword ptr [rdi + 0xa68]
    movsxd rsi, r13d
    sub rax, rsi
    imul rax, rax, 30
    cqo
    idiv rcx
    test rdx, rdx
    jns 2f
    add rdx, rcx
2:
    cmp rdx, 30
    jae 1b
    lea rax, [rdx + rcx]
    xor edx, edx
    mov ecx, 30
    div rcx
    vcvtsi2ss xmm0, xmm0, eax
    jmp 0b
    .att_syntax prefix
    .size bb_chr_due_check_stub, . - bb_chr_due_check_stub
    .popsection
)");

namespace bb::game {

namespace {

// The due check's other way out, besides its continuation.
constexpr hook::JumpTarget chr_due_targets[] = {{chr_not_due_continue, &bb_chr_due_not_due}};

}  // namespace

void set_chr_due_rate(unsigned fps) {
    bb_chr_due_rate = static_cast<std::int32_t>(fps);
    bb_chr_due_scale =
        fps % default_frame_rate == 0 ? static_cast<std::int32_t>(fps / default_frame_rate) : 0;
}

void world_chr_update_with(WorldChrUpdate* original, float step, void* world) {
    set_chr_due_rate(frame_rate());
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
