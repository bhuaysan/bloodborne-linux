// Vector ALU formats: opcode tables and decoding (internal to bbgcn).
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "gpu/gcn/gcn.hpp"

namespace bb::gcn::detail {

// The operands of a vector ALU opcode, in LLVM's order.
enum class Vop : std::uint8_t {
    plain,    // VOP1: vdst, src0
    none,     // no operands: v_nop, v_clrexcp
    sdst,     // an SGPR destination in the vdst field, then src0: v_readfirstlane_b32
};

// One opcode. Operand sizes are in dwords.
struct VopInfo {
    std::string name;  // LLVM's mnemonic with its _e32 suffix where it prints one; empty for
                       // opcodes the format doesn't define
    std::uint8_t dst = 0;
    std::uint8_t src0 = 0;
    std::uint8_t src1 = 0;
    Vop kind = Vop::plain;
};

// The opcode table of VOP1, indexed by opcode (empty for the other formats), written from the opcode lists of the
// Sea Islands ISA manual.
const std::vector<VopInfo>& vop_opcodes(Format format);

// Decodes the VOP1 instruction at code[index] (bit 31 clear; VOP2 and VOPC aren't decoded yet).
// Throws GcnError.
Instruction decode_vop(std::span<const std::uint32_t> code, std::size_t index);

}  // namespace bb::gcn::detail
