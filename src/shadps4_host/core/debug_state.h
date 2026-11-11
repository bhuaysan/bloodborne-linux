// shadPS4's debug state for its developer tools, as no-ops: the port has no developer tools. Only
// the rasterizer's draw and dispatch counts are kept, for bb-run's log.
#pragma once

#include <atomic>
#include <utility>

#include "common/types.h"

namespace DebugStateType {

class DebugStateImpl {
public:
    // Written by the presenter. ImeDialog sizes its window with them.
    std::pair<u32, u32> game_resolution{};
    std::pair<u32, u32> output_resolution{};
    bool is_using_fsr{};

    bool& IsShowingDebugMenuBar() { return showing_debug_menu_bar; }
    void IncFlipFrameNum() {}
    void IncDrawCall() noexcept { draws.fetch_add(1, std::memory_order_relaxed); }
    void IncDispatch() noexcept { dispatches.fetch_add(1, std::memory_order_relaxed); }
    std::atomic<u64> draws{0};
    std::atomic<u64> dispatches{0};

    // Register dumps are requested by the tools only.
    bool DumpingCurrentReg() { return false; }
    void PushRegsDump(uintptr_t, uintptr_t, const auto&) {}
    void PushRegsDumpCompute(uintptr_t, uintptr_t, const auto&) {}

    // EmulatorSettings keeps shader collection off, so this is not called.
    void CollectShader(const auto&...) {}

private:
    bool showing_debug_menu_bar = false;
};

}  // namespace DebugStateType

inline DebugStateType::DebugStateImpl DebugState;
