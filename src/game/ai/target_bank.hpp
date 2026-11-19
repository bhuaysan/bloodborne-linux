// The target bank's tick at the target frame rate (docs/design/frame-rate.md, "The target bank").
#pragma once

#include <array>
#include <cstdint>

namespace bb::game {

// The game's target bank tick, 0x012a43b0, a member functor of a per-frame task (built at
// 0x01a654b8) on the SprjTargetBankManager singleton (0x0553b098). Per call it clears the slot of
// the AI update schedule ring (100 bytes at +0x30, index at +0x94) that it leaves and moves to the
// next one, and advances a timer (+0x2c) by a literal 1/30 (0x012a4416); when the timer reaches the
// interval (+0x28, 5 s) it resets it and rebuilds the bank (+0x08 to +0x18) from the world's
// characters. The AI's think scheduler (0x012ab4c0) books slots `seconds * 30` ahead of the index,
// so the ring holds one slot per 1/30 s.
using TargetBankScanTick = void(void* manager);

// Its first 17 bytes: push rbp; mov rbp, rsp; push r15 to rbx; sub rsp, 0x28.
inline constexpr std::array<std::uint8_t, 17> target_bank_scan_tick_prologue = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41,
    0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x28};

// Runs `original` on `manager` in the calls where the 30 Hz clock game_tick(fps, phase) ticks:
// every (fps / 30)-th call at a whole multiple of 30, 30 calls a game second at any rate. At
// fps <= 30 every call runs `original`, whatever `phase` held, and `phase` becomes 0.
void target_bank_scan_tick_every(TargetBankScanTick* original, unsigned fps, unsigned& phase,
                                 void* manager);

// 0x012a43b0, wrapped: target_bank_scan_tick_every at frame_rate(), so the timer, the bank and the
// ring move on at 30 steps a game second as at 30 FPS. At 30 FPS it is the original.
void target_bank_scan_tick(void* manager);

}  // namespace bb::game
