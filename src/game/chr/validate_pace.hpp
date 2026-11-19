// How many characters the world sets up a game second, at the target frame rate
// (docs/design/frame-rate.md, "The character update interval (#573)", "Characters set up after
// the load").
#pragma once

#include <array>
#include <cstdint>

namespace bb::game {

// The game's per-frame budget of character setups: WorldChrMan's frame round (chr_frame_update,
// 0x01548740, at 0x01549483 and 0x015495c7) and 0x019191c0 (no caller found) build it on the
// stack as {limit = WorldChrMan +0x5f90, count = 0} and hand it to each character's state step
// (0x018bd320). WorldChrMan's constructor (0x01913a40) stores -1 (no limit) there and then the
// parameter "WorldChrMan.MaxValidateNum", 1 by default (0x019144dd). chr_frame_update runs once a
// frame, from WorldChrMan's step (0x0193ac10) or from load steps (0x01939bb0, 0x01939e90,
// 0x0193a2d0, 0x0193a3e0).
struct ValidateBudget {
    std::int32_t limit;  // < 0: no limit
    std::int32_t count;  // characters set up so far this frame
};

// 0x018b9890, a character's setup gate, called by its state step with the frame's budget. It
// returns false without a setup for a null character. For a character whose virtual +0x1b8 says
// false (TODO(name): what the others are), it returns false while the character's behaviour slot
// at SprjHkBehManager is in state 1 or 2, or when the budget is spent (0 <= limit <= count). For
// any character it returns false while +0x288 is set, its byte +0xd8 is not 6 and bit 0x20 of
// +0x1e1 is clear. Otherwise it sets the character up (virtual +0x60, which builds its AI agent
// among others), counts it in the budget and returns true (in al only). It reads the budget's
// limit and count and writes only the count.
using ChrValidate = bool(ValidateBudget* budget, void* chr);

// Its first 17 bytes: push rbp; mov rbp, rsp; push r15, r14, r12, rbx; mov rbx, rsi;
// mov r14, rdi.
inline constexpr std::array<std::uint8_t, 17> chr_validate_prologue = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41,
    0x54, 0x53, 0x48, 0x89, 0xf3, 0x49, 0x89, 0xfe};

// Runs `original` with `budget`, but at `fps` > 30 and with a limited budget only on the world
// frames `frame` where a 30 Hz clock of game time ticks (frame * 30 modulo fps below 30: the
// multiples of k = fps / 30 when that is whole, 4 or 5 frames apart at 144): on the others the
// original sees a budget with limit 0, so it sets up only characters the budget skips, and their
// count is added to `budget`. The limit then holds 30 times a game second, as at 30. At fps <= 30
// or with no limit it calls `original` with `budget`.
bool chr_validate_paced(ChrValidate* original, ValidateBudget* budget, void* chr,
                        std::uint32_t frame, unsigned fps);

// The WorldChrMan singleton (0x0553e878) and its frame counter, which only its frame update
// (0x01915320) adds 1 to. WorldChrMan's step (0x0193ac10) runs chr_frame_update before that
// update, so each round sees the value the previous frame's update left. The load steps
// (0x01939bb0, 0x01939e90, 0x0193a2d0, 0x0193a3e0) run the round without the update, and so does
// the step in frames where it skips the update (r12b clear at 0x0193b5ce, for instance with +0x150
// of its object set).
inline constexpr std::uint64_t world_chr_man = 0x0553e878;
inline constexpr std::uint64_t world_chr_man_frame = 0xa68;

// Which frames of setups to pace: those in which the world's time goes on, told by WorldChrMan's
// frame counter advancing. Fed once per gate call with the counter and frame_count(), it returns
// the frame number to pace by: the counter if it changed in this main loop frame (the first round
// of a frame in which the world goes on sees a new value), else 0, which the pacing lets through,
// so while the counter stands still the game's gate runs as it does, once a frame. The first
// frame after a stand-still still sees the old value and is let through too.
class WorldFrames {
public:
    std::uint32_t frame(std::uint32_t counter, std::uint32_t main_loop_frame);

private:
    bool seen_ = false;
    std::uint32_t counter_ = 0;
    std::uint32_t changed_at_ = 0;
};

// 0x018b9890, wrapped: chr_validate_paced with the game's function, the frame of a WorldFrames
// fed with WorldChrMan's counter and frame_count(), and frame_rate(). A frame count of the
// main loop alone would also pace the setups of the load steps, in which no game time passes, so
// at 60 fewer characters would be set up by the time the world runs. The counter alone can stand
// still on an odd value and stop the setups.
bool chr_validate(ValidateBudget* budget, void* chr);

}  // namespace bb::game
