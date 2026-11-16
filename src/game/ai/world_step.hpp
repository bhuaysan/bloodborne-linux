// The Havok AI world's step at the target frame rate (docs/design/frame-rate.md, "The AI step").
#pragma once

#include <array>
#include <cstdint>

#include "game/frame/frame.hpp"

namespace bb::game {

// The game's AI world step, 0x01dcdad0: `manager` is the AI world manager (SprjHkAiManager +0x10),
// `task` its task data; it reads only task->delta_time and passes it to the Havok AI world's step
// (0x008a0790 or 0x008a07a0) as the time step.
using AiWorldStep = void(void* manager, const FD4TaskData* task);

// Its first 17 bytes: push rbp; mov rbp, rsp; push r15 to rbx; sub rsp, 0x58.
inline constexpr std::array<std::uint8_t, 17> ai_world_step_prologue = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41,
    0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x58};

// The return address (link) of the call in chr_ai_step (0x01e2bc10) that hands the AI world step
// task data of its own with a literal 1/30 (0x01e2bcd1, call at 0x01e2bce6).
inline constexpr std::uint64_t chr_ai_step_call_return = 0x01e2bceb;

// Calls `original` as the call that returns to `return_address` (a runtime address) asked. When
// that is chr_ai_step's call and the frame rate isn't 30, the step gets task data with the same
// vtable and frame_step() as its delta_time instead of the literal 1/30. Every other call, and
// every call at 30, gets the caller's task data unchanged.
void ai_world_step_from(AiWorldStep* original, std::uintptr_t return_address, void* manager,
                        const FD4TaskData* task);

// 0x01dcdad0, wrapped: ai_world_step_from with the game's original and the caller's return
// address.
void ai_world_step(void* manager, const FD4TaskData* task);

}  // namespace bb::game
