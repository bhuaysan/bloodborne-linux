// Opcode tables of the GCN microcode formats (internal to bbgcn).
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "gpu/gcn/gcn.hpp"

namespace bb::gcn::detail {

// How the 16-bit immediate of SOPK and SOPP is used.
enum class Imm : std::uint8_t {
    none,
    simm,          // SOPK: hex value
    count,         // SOPP: unsigned number
    branch,        // signed branch offset
    hwreg,         // s_getreg: destination, then the hardware register
    setreg,        // s_setreg: the hardware register, then the source
    setreg_imm32,  // s_setreg_imm32: the hardware register, then a literal
    waitcnt,
    sendmsg,
};

// One opcode. The operand sizes are in dwords, 0 when the operand is absent. In SMRD, src0 is the
// base address (2 dwords, 4 for buffer loads) and src1 the offset (1, or 0 when it has none).
struct OpInfo {
    std::string name;  // empty for opcodes the format doesn't define
    std::uint8_t dst = 0;
    std::uint8_t src0 = 0;
    std::uint8_t src1 = 0;
    Imm imm = Imm::none;
};

// The opcode table of a scalar format, indexed by opcode, written from the opcode lists of the
// Sea Islands ISA manual. Empty for formats that aren't decoded yet.
const std::vector<OpInfo>& opcodes(Format format);

// DS and FLAT (ds_flat.cpp). Both are 2 dwords long and have their own opcode tables.
bool is_ds_flat(std::uint32_t word);
Instruction decode_ds_flat(std::span<const std::uint32_t> code, std::size_t index);
std::string ds_flat_modifiers(const Instruction& inst);  // " offset:16 gds", or empty

// The operand with 8-bit operand code `c` and `dwords` dwords of the instruction at code[index],
// as in a scalar source field. A literal comes from code[index + 1]. Throws GcnError.
Operand scalar_operand(std::span<const std::uint32_t> code, std::size_t index, unsigned c,
                       std::uint8_t dwords);

}  // namespace bb::gcn::detail
