// The ALU emitters of alu.hpp.

#include "gpu/gcn/alu.hpp"

#include <limits>

#include <spirv/unified1/GLSL.std.450.h>

namespace bb::gcn::detail {

namespace {

using Op = spv::Op;

struct MulName {
    std::string_view name;
    IntMul op;
};
constexpr MulName mul_names[] = {
    {"v_mul_lo_u32", {32, false, false, false}},  {"v_mul_hi_u32", {32, false, true, false}},
    {"v_mul_lo_i32", {32, true, false, false}},   {"v_mul_hi_i32", {32, true, true, false}},
    {"v_mul_u32_u24", {24, false, false, false}}, {"v_mul_hi_u32_u24", {24, false, true, false}},
    {"v_mul_i32_i24", {24, true, false, false}},  {"v_mul_hi_i32_i24", {24, true, true, false}},
    {"v_mad_u32_u24", {24, false, false, true}},  {"v_mad_i32_i24", {24, true, false, true}},
};

struct MathName {
    std::string_view name;
    FloatMathOp op;
};
constexpr MathName math_names[] = {
    {"v_rcp_f32", {FloatMath::rcp}},
    {"v_rcp_iflag_f32", {FloatMath::rcp}},
    {"v_rcp_clamp_f32", {FloatMath::rcp, true}},
    {"v_rcp_legacy_f32", {FloatMath::rcp, false, true}},
    {"v_rsq_f32", {FloatMath::rsq}},
    {"v_rsq_clamp_f32", {FloatMath::rsq, true}},
    {"v_sqrt_f32", {FloatMath::sqrt}},
    {"v_exp_f32", {FloatMath::exp}},
    {"v_log_f32", {FloatMath::log}},
    {"v_sin_f32", {FloatMath::sin}},
    {"v_cos_f32", {FloatMath::cos}},
};

// The valid input of v_sin_f32 and v_cos_f32 is [-256, 256] turns; beyond it sin is 0 and cos 1.
constexpr float turn_limit = 256.0f;
constexpr float two_pi = 6.28318530717958647692f;

}  // namespace

std::optional<FloatMathOp> classify_float_math(std::string_view name) {
    for (const auto& entry : math_names) {
        if (entry.name == name) {
            return entry.op;
        }
    }
    return std::nullopt;
}

std::optional<IntMul> classify_int_mul(std::string_view name) {
    for (const auto& entry : mul_names) {
        if (entry.name == name) {
            return entry.op;
        }
    }
    return std::nullopt;
}

SpirvBuilder::Id Alu::int_mul(const IntMul& op, Id a, Id b, Id c) {
    const Id u32 = b_.type_int(32, false);
    const Id u64 = b_.type_int(64, false);
    const auto field = [&](Id v) {  // the low 24 bits, sign- or zero-extended
        if (op.bits == 32) {
            return v;
        }
        const Id shift = b_.constant_u32(8);
        return b_.op(op.is_signed ? Op::OpShiftRightArithmetic : Op::OpShiftRightLogical, u32,
                     {b_.op(Op::OpShiftLeftLogical, u32, {v, shift}), shift});
    };
    a = field(a);
    b = field(b);
    Id r;
    if (op.high) {  // the product of the sources extended to 64 bits is exact
        const Op extend = op.is_signed ? Op::OpSConvert : Op::OpUConvert;
        const Id product =
            b_.op(Op::OpIMul, u64, {b_.op(extend, u64, {a}), b_.op(extend, u64, {b})});
        r = b_.op(Op::OpUConvert, u32,
                  {b_.op(Op::OpShiftRightLogical, u64, {product, b_.constant_u32(32)})});
    } else {
        r = b_.op(Op::OpIMul, u32, {a, b});
    }
    return op.add ? b_.op(Op::OpIAdd, u32, {r, c}) : r;
}

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

SpirvBuilder::Id Alu::float_math(const FloatMathOp& op, Id x) {
    const Id f32 = b_.type_float(32);
    const Id u32 = b_.type_int(32, false);
    const Id bool_t = b_.type_bool();
    const float inf = std::numeric_limits<float>::infinity();
    const auto c = [&](float v) { return b_.constant_f32(v); };
    const auto glsl = [&](GLSLstd450 f, Id a) {
        return b_.op(Op::OpExtInst, f32, {b_.import_extended("GLSL.std.450"), f, a});
    };
    const auto select = [&](Id cond, Id a, Id b) { return b_.op(Op::OpSelect, f32, {cond, a, b}); };
    const auto test = [&](Op o, Id a, Id b) { return b_.op(o, bool_t, {a, b}); };
    const auto is_inf = [&](Id v) { return b_.op(Op::OpIsInf, bool_t, {v}); };
    // A float of the sign of `v` with the magnitude bits `magnitude`.
    const auto with_sign = [&](Id v, std::uint32_t magnitude) {
        const Id sign = b_.op(Op::OpBitwiseAnd, u32,
                              {b_.op(Op::OpBitcast, u32, {v}), b_.constant_u32(0x8000'0000)});
        return b_.op(Op::OpBitcast, f32,
                     {b_.op(Op::OpBitwiseOr, u32, {sign, b_.constant_u32(magnitude)})});
    };
    const Id nan = c(std::numeric_limits<float>::quiet_NaN());
    const auto negative = [&] { return test(Op::OpFOrdLessThan, x, c(0.0f)); };
    const auto zero = [&] { return test(Op::OpFOrdEqual, x, c(0.0f)); };
    const auto finite_positive = [&] {
        const Id above_zero = test(Op::OpFOrdGreaterThan, x, c(0.0f));
        return b_.op(Op::OpLogicalAnd, bool_t, {above_zero, test(Op::OpFOrdLessThan, x, c(inf))});
    };
    // Vulkan bounds OpFDiv only for a divisor of magnitude in [2^-126, 2^126], so +-0 (+-inf)
    // and +-inf (+-0) are selected.
    const auto recip = [&] {
        return select(zero(), with_sign(x, 0x7f80'0000),
                      select(is_inf(x), with_sign(x, 0), b_.op(Op::OpFDiv, f32, {c(1.0f), x})));
    };
    Id r = 0;
    switch (op.fn) {
    case FloatMath::rcp:  // exactly 1 for 1, which OpFDiv need not give (V_RCP_LEGACY_F32's code)
        r = select(test(Op::OpFOrdEqual, x, c(1.0f)), c(1.0f), recip());
        break;
    case FloatMath::rsq:  // +-0 gives +-infinity, +infinity 0 and a NaN itself, as recip does
        r = select(finite_positive(), glsl(GLSLstd450InverseSqrt, x),
                   select(negative(), nan, recip()));
        break;
    case FloatMath::sqrt:  // +-0, +infinity and a NaN are their own root
        r = select(finite_positive(), glsl(GLSLstd450Sqrt, x), select(negative(), nan, x));
        break;
    case FloatMath::log:  // +-0 gives -infinity, +infinity and a NaN themselves
        r = select(finite_positive(), glsl(GLSLstd450Log2, x),
                   select(negative(), nan, select(zero(), c(-inf), x)));
        // exactly 0 for 1, which Log2 need not give (the code of V_LOG_CLAMP_F32)
        r = select(test(Op::OpFOrdEqual, x, c(1.0f)), c(0.0f), r);
        break;
    case FloatMath::exp: {  // exactly 1 for +-0 (the manual's pseudo code)
        // Vulkan's 3 + 2|x| ULP bound of Exp2 says nothing about a result that overflows or
        // underflows, so those are selected: +inf from 128 up (and for +inf), 0 from -150 down,
        // where IEEE rounds to 0 (and for -inf). A NaN stays a NaN.
        r = select(test(Op::OpFOrdGreaterThanEqual, x, c(128.0f)), c(inf),
                   select(test(Op::OpFOrdLessThanEqual, x, c(-150.0f)), c(0.0f),
                          select(b_.op(Op::OpIsNan, bool_t, {x}), x, glsl(GLSLstd450Exp2, x))));
        r = select(zero(), c(1.0f), r);
        break;
    }
    case FloatMath::sin:
    case FloatMath::cos: {
        // The angle is in turns. x - roundeven(x) is exact and in [-0.5, 0.5], so the GLSL
        // function gets an angle in [-pi, pi], where its precision is defined.
        const bool sin = op.fn == FloatMath::sin;
        const Id turn = b_.op(Op::OpFSub, f32, {x, glsl(GLSLstd450RoundEven, x)});
        r = glsl(sin ? GLSLstd450Sin : GLSLstd450Cos,
                 b_.op(Op::OpFMul, f32, {turn, c(two_pi)}));
        const Id beyond = test(Op::OpFOrdGreaterThan, glsl(GLSLstd450FAbs, x), c(turn_limit));
        r = select(beyond, c(sin ? 0.0f : 1.0f), r);  // a NaN stays a NaN
        break;
    }
    }
    if (op.legacy) {  // rcp: +-0 gives +0, and an infinite result a zero of its sign
        r = select(zero(), c(0.0f), select(is_inf(r), with_sign(r, 0), r));
    }
    if (op.clamp) {  // an infinite result becomes the largest float of its sign
        r = select(is_inf(r), with_sign(r, 0x7f7f'ffff), r);
    }
    return r;
}

SpirvBuilder::Id Alu::pack_half_rtz(Id low, Id high) {
    const Id f32 = b_.type_float(32);
    const Id u32 = b_.type_int(32, false);
    const Id bool_t = b_.type_bool();
    const auto c = [&](std::uint32_t v) { return b_.constant_u32(v); };
    const auto u = [&](Op o, Id a, Id b) { return b_.op(o, u32, {a, b}); };
    const auto pick = [&](Op o, Id a, std::uint32_t limit, Id then, Id otherwise) {
        return b_.op(Op::OpSelect, u32, {b_.op(o, bool_t, {a, c(limit)}), then, otherwise});
    };
    const auto half = [&](Id x) {
        const Id bits = b_.op(Op::OpBitcast, u32, {x});
        const Id sign = u(Op::OpBitwiseAnd, u(Op::OpShiftRightLogical, bits, c(16)), c(0x8000));
        const Id magnitude = u(Op::OpBitwiseAnd, bits, c(0x7fff'ffff));
        // Below 2^-14 the half is subnormal: the magnitude in units of 2^-24, truncated. The
        // product is exact.
        const Id abs = b_.op(Op::OpExtInst, f32,
                             {b_.import_extended("GLSL.std.450"), GLSLstd450FAbs, x});
        const Id subnormal = b_.op(Op::OpConvertFToU, u32,
                                   {b_.op(Op::OpFMul, f32, {abs, b_.constant_f32(0x1p24f)})});
        // A normal half: rebias the exponent from 127 to 15 and drop 13 mantissa bits.
        const Id normal = u(Op::OpShiftRightLogical, u(Op::OpISub, magnitude, c(0x3800'0000)),
                            c(13));
        Id h = pick(Op::OpULessThan, magnitude, 0x3880'0000, subnormal, normal);
        // From 65536 up a finite value truncates to the largest half, 65504.
        h = pick(Op::OpUGreaterThanEqual, magnitude, 0x4780'0000, c(0x7bff), h);
        h = pick(Op::OpIEqual, magnitude, 0x7f80'0000, c(0x7c00), h);
        h = pick(Op::OpUGreaterThan, magnitude, 0x7f80'0000, c(0x7e00), h);  // a quiet NaN
        return u(Op::OpBitwiseOr, sign, h);
    };
    return u(Op::OpBitwiseOr, half(low), u(Op::OpShiftLeftLogical, half(high), c(16)));
}

SpirvBuilder::Id Alu::half_to_float(Id bits) {
    const Id f32 = b_.type_float(32);
    const Id pair = b_.op(Op::OpExtInst, b_.type_vector(f32, 2),
                          {b_.import_extended("GLSL.std.450"), GLSLstd450UnpackHalf2x16, bits});
    return b_.op(Op::OpCompositeExtract, f32, {pair, 0});
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
