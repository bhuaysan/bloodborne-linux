// The world's character update intervals at the target frame rate (docs/design/frame-rate.md,
// "The character update interval (#573)").
#pragma once

#include <array>
#include <cstdint>

namespace bb::game {

// The game's character update round, 0x019173b0, called once a frame by WorldChrMan's frame
// update (0x01915320, call at 0x01916b3f) with the frame's step and WorldChrMan. It walks the
// world's character groups (+0x278 groups at +0x280, 0x18 bytes each) and lists the characters
// due this frame with their step: a character whose update interval (chr+0xe4, in frames) is 0
// every frame with the frame's step; one with an interval u > 0 when
//   index in its group % u == frame counter (WorldChrMan +0xa68, +1 a frame) % u,
// with u times the frame's step (0x01917909 to 0x0191792c). Then it runs the listed characters'
// updates, on the CSChrThreads among others. The interval is also a level of detail that the
// characters' own code compares with fixed values (0, 2, 5, 30 and up to 5 in switches), and this
// function keeps it in [rbp-0xc8] for its own counts.
using WorldChrUpdate = void(float step, void* world);

// Its first 20 bytes: push rbp; mov rbp, rsp; push r15 to rbx; sub rsp, 0xc8.
inline constexpr std::array<std::uint8_t, 20> world_chr_update_prologue = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
    0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0xc8, 0x00, 0x00, 0x00};

// The due check at 0x01917909, entered with ecx = u > 0, r13d = the index, rdi = WorldChrMan,
// only by the jne at 0x01917764 (no other jump of the game lands in these bytes):
//   mov eax, r13d; cdq; idiv ecx; mov esi, edx          (index % u)
//   mov eax, [rdi+0xa68]; xor edx, edx; div ecx         (frame counter % u)
//   cmp esi, edx; jne 0x01917a1b                         (not due: stores u to [rbp-0xc8])
//   vcvtsi2ss xmm0, xmm0, ecx; mov [rbp-0xc8], ecx       (due: u, then * the step at 0x0191792d)
inline constexpr std::array<std::uint8_t, 36> chr_due_check_patched = {
    0x44, 0x89, 0xe8, 0x99, 0xf7, 0xf9, 0x89, 0xd6, 0x8b, 0x87, 0x68, 0x0a,
    0x00, 0x00, 0x31, 0xd2, 0xf7, 0xf1, 0x39, 0xd6, 0x0f, 0x85, 0xf8, 0x00,
    0x00, 0x00, 0xc5, 0xfa, 0x2a, 0xc1, 0x89, 0x8d, 0x38, 0xff, 0xff, 0xff};

inline constexpr std::uint64_t chr_due_check = 0x01917909;
inline constexpr std::uint64_t chr_due_continue = 0x0191792d;  // vmulss xmm0, xmm0, [rbp-0xb0]
inline constexpr std::uint64_t chr_not_due_continue = 0x01917a1b;  // mov [rbp-0xc8], ecx
// hook::apply jumps the stub back behind the patched bytes.
static_assert(chr_due_continue == chr_due_check + chr_due_check_patched.size());

// Sets the factor of the intervals in the due check's stub, frame_rate() / 30: with `scale` k a
// character with interval u > 0 is due every k * u frames with k * u times the frame's step, which
// at k frames a 30 FPS frame is every u frames and u steps of 30 FPS. The level of detail
// ([rbp-0xc8], chr+0xe4) stays u. At k = 1 the stub does what the game's bytes do.
void set_chr_due_scale(std::int32_t scale);

// Sets the due check for frame_rate(), then runs `original`.
void world_chr_update_with(WorldChrUpdate* original, float step, void* world);

// 0x019173b0, wrapped: world_chr_update_with the game's function.
void world_chr_update(float step, void* world);

}  // namespace bb::game

// The stub that replaces the due check (BB_PATCH), written in assembly for that place, and what it
// reads: the scale and the absolute addresses of chr_due_continue and chr_not_due_continue, which
// hook::apply fills.
extern "C" {
void bb_chr_due_check_stub();
extern std::int32_t bb_chr_due_scale;
extern std::uint64_t bb_chr_due_continue;
extern std::uint64_t bb_chr_due_not_due;
}
