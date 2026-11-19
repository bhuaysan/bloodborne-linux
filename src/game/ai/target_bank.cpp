// The target bank's tick at the target frame rate (docs/design/frame-rate.md, "The target bank"): a
// wrapper that runs the game's tick 30 times a game second, whatever the frame rate.
#include "game/ai/target_bank.hpp"

#include "game/frame/frame.hpp"
#include "game/image.hpp"
#include "hook/hook.hpp"

namespace bb::game {

void target_bank_scan_tick_every(TargetBankScanTick* original, unsigned fps, unsigned& phase,
                                 void* manager) {
    if (game_tick(fps, phase)) {
        original(manager);
    }
}

// 0x012a43b0
void target_bank_scan_tick(void* manager) {
    // The game's task calls the tick once a frame, from one thread.
    static unsigned phase = 0;
    target_bank_scan_tick_every(
        reinterpret_cast<TargetBankScanTick*>(hook::original(image(), 0x012a43b0)), frame_rate(),
        phase, manager);
}

}  // namespace bb::game

BB_WRAP(0x012a43b0, bb::game::target_bank_scan_tick, bb::game::target_bank_scan_tick_prologue);
