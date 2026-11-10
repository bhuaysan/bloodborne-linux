// shadPS4's debug state for its developer tools, as no-ops: the port has no developer tools.
#pragma once

#include <utility>

#include "common/types.h"

namespace DebugStateType {

class DebugStateImpl {
public:
    // Written by the presenter for the tools' statistics and never read here.
    std::pair<u32, u32> game_resolution{};
    std::pair<u32, u32> output_resolution{};
    bool is_using_fsr{};

    bool& IsShowingDebugMenuBar() { return showing_debug_menu_bar; }
    void IncFlipFrameNum() {}
    void IncDrawCall() noexcept {}
    void IncDispatch() noexcept {}

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
