// How many characters the world sets up a game second, at the target frame rate
// (docs/design/frame-rate.md, "The character update interval (#573)", "Characters set up after
// the load"): the game's budget of setups counts per frame; at 60 FPS it counts per 30 FPS frame.
#include "game/chr/validate_pace.hpp"

#include "game/frame/frame.hpp"
#include "game/image.hpp"
#include "hook/hook.hpp"

namespace bb::game {

bool chr_validate_paced(ChrValidate* original, ValidateBudget* budget, void* chr,
                        std::uint32_t frame, unsigned fps) {
    // The 30 Hz clock ticks in the world frames whose phase, frame * 30 modulo fps, is below 30:
    // every (fps / 30)-th frame at a whole multiple of 30, as game_tick counts calls.
    if (fps <= default_frame_rate || budget->limit < 0 ||
        std::uint64_t{frame} * default_frame_rate % fps < default_frame_rate) {
        return original(budget, chr);
    }
    ValidateBudget spent{0, budget->count};
    const bool result = original(&spent, chr);
    budget->count = spent.count;
    return result;
}

std::uint32_t WorldFrames::frame(std::uint32_t counter, std::uint32_t main_loop_frame) {
    if (!seen_ || counter != counter_) {
        seen_ = true;
        counter_ = counter;
        changed_at_ = main_loop_frame;
    }
    return main_loop_frame == changed_at_ ? counter : 0;
}

// 0x018b9890
bool chr_validate(ValidateBudget* budget, void* chr) {
    auto* const original = reinterpret_cast<ChrValidate*>(hook::original(image(), 0x018b9890));
    const unsigned fps = frame_rate();
    const char* const world = global<const char*>(world_chr_man);
    if (fps <= default_frame_rate || world == nullptr) {
        return original(budget, chr);
    }
    // Only the game's main thread runs the gate (its callers run in WorldChrMan's step and the
    // load steps).
    static WorldFrames frames;
    const std::uint32_t counter =
        *reinterpret_cast<const std::uint32_t*>(world + world_chr_man_frame);
    return chr_validate_paced(original, budget, chr, frames.frame(counter, frame_count()), fps);
}

}  // namespace bb::game

BB_WRAP(0x018b9890, bb::game::chr_validate, bb::game::chr_validate_prologue);
