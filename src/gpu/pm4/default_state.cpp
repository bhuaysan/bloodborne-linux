// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/core/libraries/gnmdriver/gnmdriver.cpp (ClearContextState,
//   sceGnmDrawInitDefaultHardwareState200, sceGnmDispatchInitDefaultHardwareState),
//   src/core/libraries/gnmdriver/gnmdriver_init.h (InitSequence200)
//   @0d5f2739d2544d2a35c73e354cba7a32b7a81bea
// Changed for bloodborne-linux, 2026-11-06: the sequences as data with our packet headers, the
// padding as one function.
#include "gpu/pm4/default_state.hpp"

#include <algorithm>
#include <array>

#include "gpu/pm4/packet.hpp"

namespace bb::pm4 {

namespace {

constexpr std::uint32_t set_sh = 0xc0017600;       // SET_SH_REG, one register
constexpr std::uint32_t set_sh_cs = 0xc0017602;    // the same with the shader-type bit
constexpr std::uint32_t set_context = 0xc0016900;  // SET_CONTEXT_REG, one register

constexpr std::array draw_state{
    // ClearContextState: CONTEXT_CONTROL, CLEAR_STATE, ACQUIRE_MEM.
    0xc0012800u, 0x80000000u, 0x80000000u, 0xc0001200u, 0u, 0xc0055800u,
    0x2ec47fc0u, 0xffffffffu, 0u, 0u, 0u, 10u,
    // InitSequence200 without its CLEAR_STATE preamble.
    set_sh, 0x216u, 0xffffffffu, set_sh, 0x217u, 0xffffffffu,
    set_sh, 0x215u, 0u, set_context, 0x2f9u, 0x2du,
    set_context, 0x282u, 8u, set_context, 0x280u, 0x80008u,
    set_context, 0x281u, 0xffff0000u, set_context, 0x204u, 0u,
    set_context, 0x206u, 0x43fu, set_context, 0x83u, 0xffffu,
    set_context, 0x317u, 0x10u, set_context, 0x2fau, 0x3f800000u,
    set_context, 0x2fcu, 0x3f800000u, set_context, 0x2fbu, 0x3f800000u,
    set_context, 0x2fdu, 0x3f800000u, set_context, 0x202u, 0xcc0010u,
    set_context, 0x30eu, 0xffffffffu, set_context, 0x30fu, 0xffffffffu,
    0xc0002f00u, 1u,  // NUM_INSTANCES
    set_sh, 7u, 0x1701ffu, set_sh, 0x46u, 0x1701fdu,
    set_sh, 0x87u, 0x1701ffu, set_sh, 0xc7u, 0x1701fdu,
    set_sh, 0x107u, 0x17u, set_sh, 0x147u, 0x1701fdu,
    set_sh, 0x47u, 0x1cu, set_context, 0x1b1u, 2u,
    set_context, 0x101u, 0u, set_context, 0x100u, 0xffffffffu,
    set_context, 0x103u, 0u, set_context, 0x284u, 0u,
    set_context, 0x290u, 0u, set_context, 0x2aeu, 0u,
    set_context, 0x292u, 0u, set_context, 0x293u, 0x6020000u,
    set_context, 0x2f8u, 0u, set_context, 0x2deu, 0x1e9u,
    0xc0036900u, 0x295u, 0x100u, 0x100u, 4u,
    0xc0017900u, 0x200u, 0xe0000000u,  // SET_UCONFIG_REG GRBM_GFX_INDEX: broadcast
};
static_assert(draw_state.size() == 12 + 0x76);

constexpr std::array dispatch_state{
    set_sh_cs, 0x216u, 0xffffffffu,  // COMPUTE_STATIC_THREAD_MGMT_SE0
    set_sh_cs, 0x217u, 0xffffffffu,  // COMPUTE_STATIC_THREAD_MGMT_SE1
    set_sh_cs, 0x215u, 0x170u,       // COMPUTE_RESOURCE_LIMITS
    0xc0055800u, 0x28000000u, 0u, 0u, 0u, 0u, 0xau,  // ACQUIRE_MEM
};

}  // namespace

std::span<const std::uint32_t> draw_default_state() { return draw_state; }

std::span<const std::uint32_t> dispatch_default_state() { return dispatch_state; }

std::uint32_t write_default_state(std::uint32_t* cmd, std::uint32_t dwords,
                                  std::span<const std::uint32_t> state) {
    if (cmd == nullptr || dwords < default_state_dwords) {
        return 0;
    }
    std::uint32_t* const nop = std::ranges::copy(state, cmd).out;
    const auto body = static_cast<std::uint32_t>(cmd + default_state_dwords - nop - 1);
    nop[0] = type3_header(IT_NOP, body);
    nop[1] = 0;
    return default_state_dwords;
}

}  // namespace bb::pm4
