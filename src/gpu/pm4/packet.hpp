// PM4 packet headers (docs/design/graphics.md, "Command buffers to the first frame"). Opcode
// numbers are AMD's IT_* values (third_party/amd-gcn/si_ci_vi_merged_pm4_it_opcodes.h).
//
// A header's type is in bits 30-31. Type 3 holds the predicate bit (0), the shader-type bit (1),
// the opcode (bits 8-15) and the body length minus one (bits 16-29); a count of 0x3FFF stands
// for an empty body. Type 2 is a one-dword filler. Type 0 writes count + 1 registers from the
// base offset in bits 0-15.
#pragma once

#include <cstdint>
#include <optional>
#include <span>

#include "si_ci_vi_merged_pm4_it_opcodes.h"

namespace bb::pm4 {

enum class ShaderType : std::uint8_t { graphics = 0, compute = 1 };

inline constexpr std::uint32_t type2_filler = 0x80000000;

constexpr std::uint32_t header_type(std::uint32_t header) { return header >> 30; }

// Body dwords of a type-0 or type-3 header.
constexpr std::uint32_t body_dwords(std::uint32_t header) {
    return ((header >> 16 & 0x3FFF) + 1) & 0x3FFF;
}

// A type-3 header with `body` (0 to 0x3FFF) body dwords.
constexpr std::uint32_t type3_header(std::uint8_t opcode, std::uint32_t body,
                                     ShaderType shader = ShaderType::graphics,
                                     bool predicate = false) {
    return 3u << 30 | ((body - 1) & 0x3FFF) << 16 | std::uint32_t{opcode} << 8 |
           static_cast<std::uint32_t>(shader) << 1 | (predicate ? 1u : 0u);
}

// One type-3 packet of a stream.
struct Packet {
    std::uint32_t header = 0;
    std::span<const std::uint32_t> body;

    std::uint8_t opcode() const { return header >> 8 & 0xFF; }
    bool predicate() const { return (header & 1) != 0; }
    ShaderType shader_type() const { return static_cast<ShaderType>(header >> 1 & 1); }
};

// The tag of a NOP whose first body dword is 0x6875xxxx (markers, flip requests and other
// driver notes the parser skips), else nothing.
inline std::optional<std::uint32_t> nop_tag(const Packet& packet) {
    if (packet.opcode() != IT_NOP || packet.body.empty() || packet.body[0] >> 16 != 0x6875) {
        return std::nullopt;
    }
    return packet.body[0];
}

}  // namespace bb::pm4
