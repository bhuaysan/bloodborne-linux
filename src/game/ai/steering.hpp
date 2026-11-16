// The AI agents' steering at the target frame rate (docs/design/frame-rate.md, "The steering
// ring (#570)").
#pragma once

#include <array>
#include <cstdint>

#include "game/frame/frame.hpp"

namespace bb::game {

// The point an agent steers at: four floats (x, y, z, w) that the game returns in rax:rdx. Two
// integers, so that our functions return it in rax:rdx too.
struct SteerPoint {
    std::uint64_t low;   // x, y
    std::uint64_t high;  // z, w
};

// The game's agent steering, 0x01dbbd00: virtual +0x50 of the AI agent (vtable 0x0534e220, its
// only reference), called by the path follower (0x01239eb0) once or twice a frame per agent with
// the frame's task data. With the agent's Havok AI character (+0x230) it stores the step
// (delta_time) at +0x280, sets +0x28c (read and cleared by the AI world step), runs a state machine
// with a timer at +0x25c (in one state counted down by a literal 1/30 a call), and writes the new
// direction into a ring of 30 slots (+0x30, write index +0x24c). It reads back the average of the
// slots from a read index (+0x248) that lags `+0x250 * (1/30) / delta_time` calls behind, stores
// it at +0x20 and returns the character's position (+0x20) plus 3 times it, w = 3 * w + 1. +0x250
// counts 30 FPS frames, so at 60 the lag is twice as many calls: from 15 on it reaches the ring's
// size, the read index meets the write index every 30 calls, the average is empty and the agent
// steers at its own position. Without a character it returns a constant (0x055405a0).
using AgentSteer = SteerPoint(void* agent, const FD4TaskData* task);

// Its first 20 bytes: push rbp; mov rbp, rsp; push r15 to rbx; sub rsp, 0xb8.
inline constexpr std::array<std::uint8_t, 20> agent_steer_prologue = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
    0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0xb8, 0x00, 0x00, 0x00};

// Runs `original` as the game does at 30 FPS when the frame rate is `calls` times 30: on frames
// whose `frame` count is a multiple of `calls`, with the task data's step times `calls`. On the
// other frames an agent with a Havok AI character keeps its ring, state and step: it gets the two
// writes the game's steering does on every call (+0x28c = 1 and the character's +0x64 copied to
// +0x1c of the object at [[agent+0x238]+0x58]) and the point the game returns, from the character's
// position now and the direction stored at +0x20. An agent without one gets `original` as is. With
// calls <= 1 every call is `original` with the caller's task data.
SteerPoint agent_steer_every(AgentSteer* original, unsigned calls, std::uint64_t frame,
                             void* agent, const FD4TaskData* task);

// 0x01dbbd00, wrapped: agent_steer_every with frame_rate() / 30 calls and frame_count(). At 30 FPS
// it is the original.
SteerPoint agent_steer(void* agent, const FD4TaskData* task);

}  // namespace bb::game
