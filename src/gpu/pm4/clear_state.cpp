// SPDX-License-Identifier: LGPL-2.1-only
// The license text is third_party/fpPS4/LICENSE (chip/pm4_pfp.pas has no header of its own).
// Source: fpPS4 chip/pm4_pfp.pas @436b43064be4c78229500f3d3c054fc76639247d (t_pfp_ctx.clear_state)
// Changed for bloodborne-linux, 2026-11-07: the context register values as C++ data, a run of
// registers 0x80..0xD4 plus single values; the registers fpPS4 leaves unchanged are 0 here
// (docs/design/graphics.md, part 3 of #273).
#include <algorithm>
#include <array>

#include "gpu/pm4/default_state.hpp"

namespace bb::pm4 {

namespace {

// Context registers 0x80..0xD4 (0x83, 0x8E and 0x8F are not written by fpPS4 and stay 0).
constexpr std::array context_block{
    0x00000000u, 0x80000000u, 0x40004000u, 0x00000000u, 0x00000000u, 0x40004000u, 0x00000000u,
    0x40004000u, 0x00000000u, 0x40004000u, 0x00000000u, 0x40004000u, 0xaa99aaaau, 0x00000000u,
    0x00000000u, 0x00000000u, 0x80000000u, 0x40004000u, 0x00000000u, 0x00000000u, 0x80000000u,
    0x40004000u, 0x80000000u, 0x40004000u, 0x80000000u, 0x40004000u, 0x80000000u, 0x40004000u,
    0x80000000u, 0x40004000u, 0x80000000u, 0x40004000u, 0x80000000u, 0x40004000u, 0x80000000u,
    0x40004000u, 0x80000000u, 0x40004000u, 0x80000000u, 0x40004000u, 0x80000000u, 0x40004000u,
    0x80000000u, 0x40004000u, 0x80000000u, 0x40004000u, 0x80000000u, 0x40004000u, 0x80000000u,
    0x40004000u, 0x80000000u, 0x40004000u, 0x00000000u, 0x3f800000u, 0x00000000u, 0x3f800000u,
    0x00000000u, 0x3f800000u, 0x00000000u, 0x3f800000u, 0x00000000u, 0x3f800000u, 0x00000000u,
    0x3f800000u, 0x00000000u, 0x3f800000u, 0x00000000u, 0x3f800000u, 0x00000000u, 0x3f800000u,
    0x00000000u, 0x3f800000u, 0x00000000u, 0x3f800000u, 0x00000000u, 0x3f800000u, 0x00000000u,
    0x3f800000u, 0x00000000u, 0x3f800000u, 0x00000000u, 0x3f800000u, 0x00000000u, 0x3f800000u,
    0x2a00161au,
};
static_assert(context_block.size() == 0xD5 - 0x80);

struct ContextValue {
    std::uint32_t offset, value;
};
constexpr std::array context_values{
    ContextValue{0x000d, 0x40004000u}, ContextValue{0x01b6, 0x00000002u},
    ContextValue{0x0204, 0x00090000u}, ContextValue{0x0205, 0x00000004u},
    ContextValue{0x0295, 0x00000100u}, ContextValue{0x0296, 0x00000080u},
    ContextValue{0x0297, 0x00000002u}, ContextValue{0x02aa, 0x000000ffu},
    ContextValue{0x02f7, 0x00001000u}, ContextValue{0x02f9, 0x00000005u},
    ContextValue{0x02fa, 0x3f800000u}, ContextValue{0x02fb, 0x3f800000u},
    ContextValue{0x02fc, 0x3f800000u}, ContextValue{0x02fd, 0x3f800000u},
    ContextValue{0x0316, 0x0000000eu}, ContextValue{0x0317, 0x00000010u},
};

constexpr std::uint32_t context_dwords = 0x400;

constexpr std::array<std::uint32_t, context_dwords> make_context_defaults() {
    std::array<std::uint32_t, context_dwords> regs{};
    std::ranges::copy(context_block, regs.begin() + 0x80);
    for (const ContextValue& v : context_values) {
        regs[v.offset] = v.value;
    }
    return regs;
}

constexpr auto defaults = make_context_defaults();

}  // namespace

std::span<const std::uint32_t> context_defaults() { return defaults; }

}  // namespace bb::pm4
