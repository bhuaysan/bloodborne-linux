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
    binary,   // VOP2: vdst, src0, vsrc1
    cndmask,  // VOP2: vdst, src0, vsrc1, vcc
    carry_out,  // VOP2: vdst, vcc, src0, vsrc1
    carry,      // VOP2: vdst, vcc, src0, vsrc1, vcc
    readlane,   // VOP2: an SGPR destination, src0, a scalar lane select in the vsrc1 field
    writelane,  // VOP2: vdst, src0, a scalar lane select in the vsrc1 field
    madmk,      // VOP2: vdst, src0, a literal K, vsrc1
    madak,      // VOP2: vdst, src0, vsrc1, a literal K
    compare,    // VOPC: vcc, src0, vsrc1
    ternary,    // VOP3 only: vdst, src0, src1, src2 (binary: vdst, src0, src1)
    ternary_sdst,  // VOP3 only: vdst, an SGPR pair, src0, src1, src2
};

// One opcode. Operand sizes are in dwords.
struct VopInfo {
    std::string name;  // LLVM's mnemonic with its _e32 suffix where it prints one; empty for
                       // opcodes the format doesn't define
    std::uint8_t dst = 0;
    std::uint8_t src0 = 0;
    std::uint8_t src1 = 0;
    Vop kind = Vop::plain;
    bool f16_src0 = false;  // src0 is an f16 operand, with LLVM's 16-bit rule for literals
    std::uint8_t src2 = 0;  // VOP3 only
};

// The opcode table of VOP1, VOP2, VOPC or VOP3, indexed by opcode (empty for the other formats),
// written from the opcode lists of the Sea Islands ISA manual. The VOP3 table has the _e64 forms
// of the others (VOPC at 0, VOP2 at 0x100, VOP1 at 0x180) and the VOP3-only opcodes (0x140).
const std::vector<VopInfo>& vop_opcodes(Format format);

// Decodes the VOP1, VOP2 or VOPC instruction at code[index] (bit 31 clear). Throws GcnError.
Instruction decode_vop(std::span<const std::uint32_t> code, std::size_t index);

// Decodes the VOP3 instruction (2 dwords) or the VINTRP instruction at code[index] (vop3_decode.cpp).
Instruction decode_vop3(std::span<const std::uint32_t> code, std::size_t index);
Instruction decode_vintrp(std::span<const std::uint32_t> code, std::size_t index);

}  // namespace bb::gcn::detail
