// The ALU emitters of alu.hpp.

#include "gpu/gcn/alu.hpp"

namespace bb::gcn::detail {

namespace {
using Op = spv::Op;
}  // namespace

SpirvBuilder::Id Alu::bit_field(Id value, Id offset, Id width, bool is_signed) {
    const Id u32 = b_.type_int(32, false);
    const Id bool_t = b_.type_bool();
    const auto c = [&](std::uint32_t v) { return b_.constant_u32(v); };
    const auto u = [&](Op o, Id a, Id b) { return b_.op(o, u32, {a, b}); };
    const auto select = [&](Id cond, Id a, Id b) { return b_.op(Op::OpSelect, u32, {cond, a, b}); };
    const auto test = [&](Op o, Id a, Id b) { return b_.op(o, bool_t, {a, b}); };
    const Id low_bits = u(Op::OpISub,
                          u(Op::OpShiftLeftLogical, c(1), u(Op::OpBitwiseAnd, width, c(31))),
                          c(1));
    const Id mask = select(test(Op::OpUGreaterThanEqual, width, c(32)), c(~0u), low_bits);
    const Id field = u(Op::OpBitwiseAnd, u(Op::OpShiftRightLogical, value, offset), mask);
    if (!is_signed) {
        return field;
    }
    // Inside the word: move the field's top bit to bit 31 and shift back. Shift counts of 32 are
    // masked to 0, and the width-0 and width-32 cases are picked out by the selects.
    const Id end = u(Op::OpIAdd, offset, width);
    const Id left = u(Op::OpShiftLeftLogical, value,
                      u(Op::OpBitwiseAnd, u(Op::OpISub, c(32), end), c(31)));
    const Id inside = u(Op::OpShiftRightArithmetic, left,
                        u(Op::OpBitwiseAnd, u(Op::OpISub, c(32), width), c(31)));
    const Id past = u(Op::OpShiftRightArithmetic, value, offset);
    const Id r = select(test(Op::OpULessThan, end, c(32)), inside, past);
    return select(test(Op::OpIEqual, width, c(0)), c(0), r);
}

SpirvBuilder::Id Alu::quad_any(Id bit) {
    b_.capability(spv::Capability::GroupNonUniformQuad);
    const Id u32 = b_.type_int(32, false);
    const auto c = [&](std::uint32_t v) { return b_.constant_u32(v); };
    Id v = b_.op(Op::OpSelect, u32, {bit, c(1), c(0)});
    for (const std::uint32_t direction : {0u, 1u}) {  // across the row, then down the column
        const Id other = b_.op(Op::OpGroupNonUniformQuadSwap, u32,
                               {c(spirv_word(spv::Scope::Subgroup)), v, c(direction)});
        v = b_.op(Op::OpBitwiseOr, u32, {v, other});
    }
    return b_.op(Op::OpINotEqual, b_.type_bool(), {v, c(0)});
}

}  // namespace bb::gcn::detail
