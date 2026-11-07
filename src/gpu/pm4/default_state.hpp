// The default hardware state (docs/design/graphics.md, item 18): the packets that
// sceGnmDrawInitDefaultHardwareState200 and sceGnmDispatchInitDefaultHardwareState write, and the
// context register values CLEAR_STATE restores. Base PS4 (not Neo) sequences only.
#pragma once

#include <cstdint>
#include <span>

namespace bb::pm4 {

// The fixed size of both default state blocks: the game's writers (0x1072870, 0x1089c50) reserve
// it and advance the cursor by the import's result.
inline constexpr std::uint32_t default_state_dwords = 256;

// The graphics block without its padding: CONTEXT_CONTROL, CLEAR_STATE, ACQUIRE_MEM, then the
// register writes of the SDK 2.00 default state.
std::span<const std::uint32_t> draw_default_state();

// The compute block without its padding: COMPUTE_STATIC_THREAD_MGMT_SE0/SE1 and
// COMPUTE_RESOURCE_LIMITS (SET_SH_REG with the shader-type bit), then ACQUIRE_MEM.
std::span<const std::uint32_t> dispatch_default_state();

// Writes `state` at `cmd` and pads it to default_state_dwords with one NOP whose first payload
// dword is 0 and the rest left as they were. Returns default_state_dwords, or 0 without writing
// when `cmd` is null or `dwords` is smaller.
std::uint32_t write_default_state(std::uint32_t* cmd, std::uint32_t dwords,
                                  std::span<const std::uint32_t> state);

// What CLEAR_STATE writes, one value per context register from 0xA000 (RegisterMirror::
// set_context_defaults). Adapted from fpPS4 in clear_state.cpp (LGPL-2.1-only).
std::span<const std::uint32_t> context_defaults();

}  // namespace bb::pm4
