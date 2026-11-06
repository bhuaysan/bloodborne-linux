// The arithmetic of the vector and scalar ALU that takes more than one SPIR-V instruction
// (internal to bbgcn). The emitters need no translator state: they take and return ids of 32-bit
// values (floats as f32, integers as u32) and append to the current block of a SpirvBuilder.
// Semantics from the Sea Islands ISA manual, chapters "Vector ALU instructions" and "Scalar ALU
// instructions".
#pragma once

#include "gpu/gcn/spirv_builder.hpp"

namespace bb::gcn::detail {

class Alu {
public:
    using Id = SpirvBuilder::Id;

    explicit Alu(SpirvBuilder& b) : b_(b) {}

    // Bit field extract of the u32 `value`: `offset` in 0-31 and `width` in 0-32 are u32 too. The
    // unsigned form is (value >> offset) & ((1 << width) - 1). The signed form sign-extends it
    // from its top bit, and a field that reaches past bit 31 takes the sign of the value (the
    // manual's V_BFE_I32 and S_BFE_I32 text).
    Id bit_field(Id value, Id offset, Id width, bool is_signed);

    // `bit` (a bool per lane) or'ed over the four lanes of its quad: whole quad mode.
    Id quad_any(Id bit);

private:
    SpirvBuilder& b_;
};

}  // namespace bb::gcn::detail
