// The vector memory formats (internal to bbgcn), MUBUF and MTBUF so far, and EXP, in memory.cpp.
// Their instructions are two dwords; the fields that aren't operands go to Instruction::mem_offset
// and ::flags (MTBUF also ::dfmt and ::nfmt; EXP keeps its target in mem_offset).
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "gpu/gcn/gcn.hpp"

namespace bb::gcn::detail {

// Whether `word` starts an instruction of MUBUF, MTBUF or EXP.
bool is_memory_encoding(std::uint32_t word);

// Decodes the MUBUF, MTBUF or EXP instruction at code[index]. Throws GcnError.
Instruction decode_memory(std::span<const std::uint32_t> code, std::size_t index);

// The fields of a vector memory instruction that are set, as LLVM prints them after the operands.
std::string memory_fields(const Instruction& inst);

// The whole line of an EXP instruction in LLVM's syntax.
std::string export_text(const Instruction& inst);

// Shared with the scalar formats, defined in decode.cpp.
std::uint32_t bits(std::uint32_t word, unsigned low, unsigned count);
bool valid_register(unsigned c, unsigned dwords);
std::uint32_t float_constant(unsigned c, unsigned dwords);

}  // namespace bb::gcn::detail
