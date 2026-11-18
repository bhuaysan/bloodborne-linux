// How many characters the world sets up a game second, at the target frame rate
// (docs/design/frame-rate.md, "The character update interval (#573)", "Characters set up after
// the load"): the game's budget of setups counts per frame; at 60 FPS it counts per 30 FPS frame.
#include "game/chr/validate_pace.hpp"

#include "game/frame/frame.hpp"
#include "game/image.hpp"
#include "hook/hook.hpp"

namespace bb::game {

bool chr_validate_paced(ChrValidate* original, ValidateBudget* budget, void* chr,
                        std::uint32_t frame, std::int32_t scale) {
    if (scale <= 1 || budget->limit < 0 || frame % static_cast<std::uint32_t>(scale) == 0) {
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
    return main_loop_frame - changed_at_ <= 1 ? counter : 0;
}

// 0x018b9890
bool chr_validate(ValidateBudget* budget, void* chr) {
    auto* const original = reinterpret_cast<ChrValidate*>(hook::original(image(), 0x018b9890));
    const auto scale = static_cast<std::int32_t>(frame_rate() / default_frame_rate);
    const char* const world = global<const char*>(world_chr_man);
    if (scale <= 1 || world == nullptr) {
        return original(budget, chr);
    }
    // Only the game's main thread runs the gate (its callers run in WorldChrMan's step and the
    // load steps).
    static WorldFrames frames;
    const std::uint32_t counter =
        *reinterpret_cast<const std::uint32_t*>(world + world_chr_man_frame);
    return chr_validate_paced(original, budget, chr, frames.frame(counter, frame_count()), scale);
}

}  // namespace bb::game

BB_WRAP(0x018b9890, bb::game::chr_validate, bb::game::chr_validate_prologue);
