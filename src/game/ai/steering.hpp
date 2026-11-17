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
// only reference), called by the path follower (0x01239eb0) with the frame's task data, on the
// frames that step the agent (every frame or every few, per agent). With the agent's Havok AI
// character (+0x230) it stores the step (delta_time) at +0x280, sets +0x28c (read and cleared by
// the AI world step), runs a state machine with a timer at +0x25c (in one state counted down by a
// literal 1/30 a call), and writes the new direction into a ring of 30 slots (+0x30, write index
// +0x24c). It reads back the average of the
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

// The character AI control update, 0x01513c10: chr vtable +0xd0 (0x018bdb20) calls it (the only
// direct caller, 0x018bded1) with the frame's step in xmm0 and the character's control object in
// rdi (the character is the pointer at control+8). Through the AI manipulator, AI character and
// path updates it reaches `agent_steer` (0x01239ef8). The character's update interval is the
// int32 at character+0xe4 (#573).
using ChrAiControlUpdate = void(float dt, void* control);

// Its first 20 bytes: push rbp; mov rbp, rsp; push r15 to rbx; sub rsp, 0xa8.
inline constexpr std::array<std::uint8_t, 20> chr_ai_control_update_prologue = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
    0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0xa8, 0x00, 0x00, 0x00};

// The update interval of the character whose control update runs on this thread.
struct ChrUpdateNote {
    bool noted = false;  // false: no such update runs, the call comes from elsewhere
    std::int32_t interval = 0;
};

// Runs `original` with the update interval of the control object's character noted for this
// thread, and puts the previous note back after it (updates may nest).
void chr_ai_control_update_noting(ChrAiControlUpdate* original, float dt, void* control);

// 0x01513c10, wrapped: at 30 FPS the original, else chr_ai_control_update_noting.
void chr_ai_control_update(float dt, void* control);

// The note of this thread.
ChrUpdateNote noted_chr_update();

// What the wrapper keeps per agent: the count of its calls, and whether the last call that ran
// the original on an agent with a character passed a step of exactly 0.0f.
struct SteerState {
    unsigned count = 0;
    bool ran_zero_step = false;
};

// Runs `original` as the game does at 30 FPS when the frame rate is `calls` times 30, counting the
// agent's own calls in `state.count` (the path follower doesn't step every agent every frame): when
// `count` is 0, with the task data's step times `calls`. On the agent's other calls, if it has a
// Havok AI character, its ring, state and step stay as they are: it gets two of the writes the
// game's steering does on every call (+0x28c = 1 and the character's +0x64 copied to +0x1c of the
// object at [[agent+0x238]+0x58]) and the point the game returns, from the character's position
// now and the direction stored at +0x20. The third, clearing the requests at +0x289 and +0x28a,
// is left to the next call that runs `original`, which acts on them. An agent whose stored step
// (+0x280) is still 0.0f, the constructor's value, runs `original` on this call whatever `count`
// says, and `count` goes on from there, unless `ran_zero_step`: a run with a step of exactly 0
// (full hit-stop) stores 0.0f too and is no sign of a new agent, so then the count rules. An
// agent without a character gets `original` as is and a fresh state, so its first call with one
// runs `original`. Then moves `count` on, modulo `calls`.
// With calls <= 1, or `halve` false (the call comes from the update of a character with an update
// interval above 0, which is stepped at 30 FPS's rate by itself, #603), every call is `original`
// with the caller's task data and `count` is 0.
SteerPoint agent_steer_every(AgentSteer* original, unsigned calls, SteerState& state, void* agent,
                             const FD4TaskData* task, bool halve = true);

// 0x01dbbd00, wrapped: agent_steer_every with frame_rate() / 30 calls and a count per agent, kept
// in a table by the agent's address; an agent not in it starts at 0. `halve` is false inside the
// control update of a character with an update interval above 0. At 30 FPS it is the original.
SteerPoint agent_steer(void* agent, const FD4TaskData* task);

// Forgets every agent's count (for tests, whose agents reuse addresses).
void forget_agent_steer_counts();

}  // namespace bb::game
