// GCN (Sea Islands, gfx7) shader code: decoder and disassembler (docs/design/graphics.md,
// "Decoder and disassembler").
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace bb::gcn {

// An encoding the decoder doesn't know or accept. Names the byte offset and first word of the
// failing instruction.
class GcnError : public std::runtime_error {
public:
    GcnError(const std::string& message, std::uint32_t offset, std::uint32_t word);

    std::uint32_t offset() const { return offset_; }
    std::uint32_t word() const { return word_; }

private:
    std::uint32_t offset_;
    std::uint32_t word_;
};

// The microcode formats of the Sea Islands ISA.
enum class Format : std::uint8_t {
    sop2, sopk, sop1, sopc, sopp, smrd,
    vop2, vop1, vopc, vop3, vintrp, ds, mubuf, mtbuf, mimg, exp, flat,
};

enum class OperandKind : std::uint8_t {
    none,
    scalar,    // SGPR or special register (also LDS_DIRECT); `code` is the 8-bit operand code
    constant,  // inline constant; `code` is its operand code, `value` its 32-bit pattern
               // (on a 2-dword operand, an integer sign-extends it and a float is the f64 of the
               // same number, with `value` as its high dword and 0 as its low dword)
    literal,   // 32-bit literal that follows the instruction, in `value`
    offset,    // SMRD dword offset, in `value`
    simm,      // 16-bit immediate, printed in hex
    count,     // 16-bit unsigned immediate, printed in decimal up to 64 and in hex above
    branch,    // signed dword offset from the next instruction, in `value` (sign-extended)
    hwreg,     // hardware register field of s_getreg/s_setreg, in `value`
    waitcnt,   // counters of s_waitcnt, in `value`
    sendmsg,   // message of s_sendmsg, in `value`
    vector,    // VGPR; `code` is the register number, `dwords` the size of the range
    off,       // an absent address, printed as "off"
};

// Operand codes of the scalar operand field with a fixed meaning.
namespace code {
inline constexpr std::uint16_t flat_scratch = 104;  // pair 104-105 (CI)
inline constexpr std::uint16_t vcc = 106;           // pair 106-107
inline constexpr std::uint16_t tba = 108;           // pair 108-109
inline constexpr std::uint16_t tma = 110;           // pair 110-111
inline constexpr std::uint16_t ttmp0 = 112;         // ttmp0-ttmp11 are 112-123
inline constexpr std::uint16_t m0 = 124;
inline constexpr std::uint16_t exec = 126;  // pair 126-127
inline constexpr std::uint16_t vccz = 251;
inline constexpr std::uint16_t execz = 252;
inline constexpr std::uint16_t scc = 253;
inline constexpr std::uint16_t lds_direct = 254;  // vector ALU sources only
inline constexpr std::uint16_t literal = 255;
}  // namespace code

// Bits of Instruction::flags, the address modes and the cache and LDS modifiers of the memory
// formats.
namespace flag {
inline constexpr std::uint16_t gds = 1;     // DS: global data share instead of LDS
inline constexpr std::uint16_t offen = 2;   // MUBUF, MTBUF: VGPR offset
inline constexpr std::uint16_t idxen = 4;   // MUBUF, MTBUF: VGPR index
inline constexpr std::uint16_t addr64 = 8;  // MUBUF, MTBUF: 64-bit VGPR address
inline constexpr std::uint16_t glc = 16;    // MUBUF, MTBUF, FLAT
inline constexpr std::uint16_t slc = 32;    // MUBUF, MTBUF, FLAT
inline constexpr std::uint16_t tfe = 64;    // MUBUF, MTBUF, FLAT
inline constexpr std::uint16_t lds = 128;   // MUBUF: load into LDS
inline constexpr std::uint16_t done = 256;   // EXP: last export of the program
inline constexpr std::uint16_t compr = 512;  // EXP: two 16-bit values per source
inline constexpr std::uint16_t vm = 1024;    // EXP: valid mask
}  // namespace flag

struct Operand {
    OperandKind kind = OperandKind::none;
    std::uint8_t dwords = 0;  // registers covered: 1, 2, 4, 8 or 16 (scalar, constant, literal)
    std::uint16_t code = 0;   // operand code (scalar, constant)
    std::uint32_t value = 0;  // see OperandKind

    bool operator==(const Operand&) const = default;
};

// One decoded instruction. Operands are in the order of LLVM's assembly syntax.
struct Instruction {
    std::uint32_t offset = 0;  // in bytes from the start of the code
    std::uint8_t size = 0;     // in dwords, with the literal
    Format format = Format::sop2;
    std::uint16_t opcode = 0;  // the format's opcode field
    std::string_view name;     // LLVM's mnemonic
    std::uint8_t num_dst = 0;
    std::uint8_t num_src = 0;
    std::array<Operand, 2> dst;
    std::array<Operand, 4> src;
    // Memory formats: the offset field (DS: offset1 in the high byte, offset0 in the low byte,
    // read as one 16-bit offset by the instructions that take one; MUBUF, MTBUF: the 12-bit
    // unsigned byte offset), and the flag bits.
    std::uint16_t mem_offset = 0;
    std::uint16_t flags = 0;
    // MTBUF: the data format and numeric format fields. EXP: mem_offset is the target.
    std::uint8_t dfmt = 0;
    std::uint8_t nfmt = 0;
};

enum class Until {
    program_end,  // the first s_endpgm at or beyond the furthest branch target
    code_end,     // every word of the code
};

// Decodes the program at the start of `code`. With Until::program_end, `code` may extend past the
// program, and running out of code before the end is an error. Throws GcnError.
std::vector<Instruction> decode(std::span<const std::uint32_t> code,
                                Until until = Until::program_end);

// One line of LLVM's assembly syntax for gfx7, without the offset or the encoding.
std::string disassemble(const Instruction& inst);

// Byte offset of the target of a direct branch, nothing for other instructions.
std::optional<std::uint32_t> branch_target(const Instruction& inst);

bool is_endpgm(const Instruction& inst);

}  // namespace bb::gcn
