// The target bank's tick at the target frame rate (docs/design/frame-rate.md, "The target bank"): a
// wrapper that runs the game's tick 30 times a game second, whatever the frame rate.
#include "game/ai/target_bank.hpp"

#include "game/frame/frame.hpp"
#include "game/image.hpp"
#include "hook/hook.hpp"

namespace bb::game {

void target_bank_scan_tick_every(TargetBankScanTick* original, unsigned calls, unsigned& count,
                                 void* manager) {
    if (count == 0) {
        original(manager);
    }
    count = calls > 1 ? (count + 1) % calls : 0;
}

// 0x012a43b0
void target_bank_scan_tick(void* manager) {
    // The game's task calls the tick once a frame, from one thread.
    static unsigned count = 0;
    target_bank_scan_tick_every(
        reinterpret_cast<TargetBankScanTick*>(hook::original(image(), 0x012a43b0)),
        frame_rate() / default_frame_rate, count, manager);
}

}  // namespace bb::game

BB_WRAP(0x012a43b0, bb::game::target_bank_scan_tick, bb::game::target_bank_scan_tick_prologue);
