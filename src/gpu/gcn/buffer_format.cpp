// Typed buffer formats. The format and number format names and numbers are from the Sea Islands
// ISA manual. The component layouts (X in the lowest bits, little endian) are our reading of the
// names; the bit positions of 2_10_10_10 and the SNORM_OGL conversion are facts from shadPS4's
// shader recompiler, and the normalizing formulas are the Vulkan specification's (see
// docs/references.md).

#include "gpu/gcn/buffer_format.hpp"

#include <cmath>
#include <initializer_list>
#include <format>

#include <spirv/unified1/GLSL.std.450.h>

namespace bb::gcn {

namespace {

using Op = spv::Op;

constexpr FormatLayout make(unsigned bytes, std::initializer_list<FormatField> fields) {
    FormatLayout l;
    l.bytes = bytes;
    for (const FormatField& f : fields) {
        l.field[l.components++] = f;
    }
    return l;
}

constexpr FormatLayout layouts[16] = {
    {},                                                          // 0 invalid
    make(1, {{0, 0, 8}}),                                        // 8
    make(2, {{0, 0, 16}}),                                       // 16
    make(2, {{0, 0, 8}, {0, 8, 8}}),                             // 8_8
    make(4, {{0, 0, 32}}),                                       // 32
    make(4, {{0, 0, 16}, {0, 16, 16}}),                          // 16_16
    {},                                                          // 10_11_11, small floats
    {},                                                          // 11_11_10
    {},                                                          // 10_10_10_2
    make(4, {{0, 0, 10}, {0, 10, 10}, {0, 20, 10}, {0, 30, 2}}),  // 2_10_10_10
    make(4, {{0, 0, 8}, {0, 8, 8}, {0, 16, 8}, {0, 24, 8}}),     // 8_8_8_8
    make(8, {{0, 0, 32}, {1, 0, 32}}),                           // 32_32
    make(8, {{0, 0, 16}, {0, 16, 16}, {1, 0, 16}, {1, 16, 16}}),  // 16_16_16_16
    make(12, {{0, 0, 32}, {1, 0, 32}, {2, 0, 32}}),              // 32_32_32
    make(16, {{0, 0, 32}, {1, 0, 32}, {2, 0, 32}, {3, 0, 32}}),  // 32_32_32_32
    {},                                                          // reserved
};

}  // namespace

const FormatLayout* format_layout(unsigned dfmt) {
    return dfmt < 16 && layouts[dfmt].components != 0 ? &layouts[dfmt] : nullptr;
}

std::string format_error(unsigned dfmt, unsigned nf, bool store) {
    const FormatLayout* layout = format_layout(dfmt);
    if (layout == nullptr) {
        return std::format("data format {} is not supported", dfmt);
    }
    if (store && (nf == nfmt::uscaled || nf == nfmt::sscaled || nf == nfmt::snorm_ogl)) {
        return std::format("number format {} can't be stored", nf);
    }
    for (unsigned i = 0; i < layout->components; ++i) {
        const unsigned w = layout->field[i].width;
        // Floats have 16 or 32 bits, and the normalized and scaled formats fewer than 32.
        const bool bad = nf == nfmt::float_                           ? w != 16 && w != 32
                         : nf == nfmt::uint_ || nf == nfmt::sint ? false
                                                                 : w == 32;
        if (bad) {
            return std::format("number format {} of data format {} is not supported", nf, dfmt);
        }
    }
    return {};
}

using Id = FormatEmitter::Id;

Id FormatEmitter::float_to_bits(Id value) {
    return b_.op(Op::OpBitcast, u32_, {value});
}

Id FormatEmitter::extended(unsigned instruction, Id type, const SpirvBuilder::Words& operands) {
    SpirvBuilder::Words all = {b_.import_extended("GLSL.std.450"), instruction};
    all.insert(all.end(), operands.begin(), operands.end());
    return b_.op(Op::OpExtInst, type, all);
}

// One component to its value: a float's bit pattern, or the integer.
Id FormatEmitter::decode_field(unsigned nf, const FormatField& field, Id word) {
    const unsigned w = field.width;
    const auto c = [&](std::uint32_t v) { return b_.constant_u32(v); };
    const Id f32 = b_.type_float(32);
    const auto uext = [&] {
        return w == 32 ? word : b_.op(Op::OpBitFieldUExtract, u32_, {word, c(field.offset), c(w)});
    };
    const auto sext = [&] {
        return w == 32 ? word : b_.op(Op::OpBitFieldSExtract, u32_, {word, c(field.offset), c(w)});
    };
    const auto to_float = [&](Op convert, Id integer) { return b_.op(convert, f32, {integer}); };
    const auto ratio = [&](Id numerator, double max) {
        return b_.op(Op::OpFDiv, f32, {numerator, b_.constant_f32(static_cast<float>(max))});
    };
    switch (nf) {
    case nfmt::unorm:
        return float_to_bits(ratio(to_float(Op::OpConvertUToF, uext()), std::ldexp(1.0, w) - 1));
    case nfmt::snorm: {  // the most negative value is clamped to -1
        const Id f = ratio(to_float(Op::OpConvertSToF, sext()), std::ldexp(1.0, w - 1) - 1);
        return float_to_bits(extended(GLSLstd450NMax, f32, {f, b_.constant_f32(-1.0f)}));
    }
    case nfmt::uscaled:
        return float_to_bits(to_float(Op::OpConvertUToF, uext()));
    case nfmt::sscaled:
        return float_to_bits(to_float(Op::OpConvertSToF, sext()));
    case nfmt::uint_:
        return uext();
    case nfmt::sint:
        return sext();
    case nfmt::snorm_ogl: {  // (2 * c + 1) / (2^w - 1)
        const Id odd = b_.op(Op::OpIAdd, u32_,
                             {b_.op(Op::OpShiftLeftLogical, u32_, {sext(), c(1)}), c(1)});
        return float_to_bits(ratio(to_float(Op::OpConvertSToF, odd), std::ldexp(1.0, w) - 1));
    }
    default:  // float
        if (w == 32) {
            return word;
        }
        const Id half = extended(GLSLstd450UnpackHalf2x16, b_.type_vector(f32, 2), {uext()});
        return float_to_bits(b_.op(Op::OpCompositeExtract, f32, {half, 0}));
    }
}

FormatEmitter::Words FormatEmitter::decode(unsigned dfmt, unsigned nf, const Words& raw) {
    const FormatLayout& layout = *format_layout(dfmt);
    Words out;
    for (unsigned i = 0; i < 4; ++i) {
        const FormatField& f = layout.field[i];
        out[i] = i < layout.components ? decode_field(nf, f, raw[f.word]) : b_.constant_u32(0);
    }
    return out;
}

// One component to its `width` bits (in the low bits, the rest 0).
Id FormatEmitter::encode_field(unsigned nf, unsigned w, Id value) {
    const auto c = [&](std::uint32_t v) { return b_.constant_u32(v); };
    const Id f32 = b_.type_float(32);
    const Id i32 = b_.type_int(32, true);
    const std::uint32_t mask = w == 32 ? ~0u : (1u << w) - 1;
    // Clamps the float to [low, high] with NaN as 0, scales it by `scale` and rounds it to the
    // nearest integer, ties to even.
    const auto normalized = [&](float low, float high, double scale, Op convert) {
        const Id f = b_.op(Op::OpBitcast, f32, {value});
        const Id number = b_.op(Op::OpSelect, f32, {b_.op(Op::OpIsNan, b_.type_bool(), {f}),
                                                    b_.constant_f32(0.0f), f});
        const Id clamped = extended(GLSLstd450FClamp, f32,
                                    {number, b_.constant_f32(low), b_.constant_f32(high)});
        const Id scaled =
            b_.op(Op::OpFMul, f32, {clamped, b_.constant_f32(static_cast<float>(scale))});
        const Id rounded = extended(GLSLstd450RoundEven, f32, {scaled});
        return b_.op(Op::OpBitwiseAnd, u32_, {b_.op(convert, u32_, {rounded}), c(mask)});
    };
    switch (nf) {
    case nfmt::unorm:
        return normalized(0.0f, 1.0f, std::ldexp(1.0, w) - 1, Op::OpConvertFToU);
    case nfmt::snorm:
        return normalized(-1.0f, 1.0f, std::ldexp(1.0, w - 1) - 1, Op::OpConvertFToS);
    case nfmt::uint_:
        return w == 32 ? value : extended(GLSLstd450UMin, u32_, {value, c(mask)});
    case nfmt::sint: {
        if (w == 32) {
            return value;
        }
        const Id low = b_.op(Op::OpBitcast, i32, {c(~0u << (w - 1))});
        const Id high = b_.op(Op::OpBitcast, i32, {c((1u << (w - 1)) - 1)});
        const Id s = b_.op(Op::OpBitcast, i32, {value});
        const Id clamped = extended(GLSLstd450SClamp, i32, {s, low, high});
        return b_.op(Op::OpBitwiseAnd, u32_, {b_.op(Op::OpBitcast, u32_, {clamped}), c(mask)});
    }
    default:  // float: 16 or 32 bits (format_error)
        return w == 32 ? value : to_half(value);
    }
}

// The half float nearest to the f32 with the bits `bits`, ties to even, in integer operations:
// values from 65520 up become infinities, the ones below 2^-14 denormals, and a NaN becomes the
// quiet NaN of its sign.
Id FormatEmitter::to_half(Id bits) {
    const auto c = [&](std::uint32_t v) { return b_.constant_u32(v); };
    const auto u = [&](Op op, Id x, Id y) { return b_.op(op, u32_, {x, y}); };
    const auto test = [&](Op op, Id x, std::uint32_t y) {
        return b_.op(op, b_.type_bool(), {x, c(y)});
    };
    const auto pick = [&](Id cond, Id x, Id y) { return b_.op(Op::OpSelect, u32_, {cond, x, y}); };
    // x >> s rounded to the nearest, ties to even, for 1 <= s <= 31 and x < 2^31.
    const auto round_shift = [&](Id x, Id s) {
        const Id odd = u(Op::OpBitwiseAnd, u(Op::OpShiftRightLogical, x, s), c(1));
        const Id below_half =
            u(Op::OpISub, u(Op::OpShiftLeftLogical, c(1), u(Op::OpISub, s, c(1))), c(1));
        return u(Op::OpShiftRightLogical, u(Op::OpIAdd, u(Op::OpIAdd, x, below_half), odd), s);
    };
    const Id sign = u(Op::OpShiftRightLogical, u(Op::OpBitwiseAnd, bits, c(0x80000000)), c(16));
    const Id magnitude = u(Op::OpBitwiseAnd, bits, c(0x7fffffff));
    // A normal half: rebias the exponent from 127 to 15 and drop 13 mantissa bits.
    const Id normal = round_shift(u(Op::OpISub, magnitude, c(112u << 23)), c(13));
    // A denormal: the mantissa with its leading 1, shifted by 126 - exponent (14 or more).
    const Id exponent = u(Op::OpShiftRightLogical, magnitude, c(23));
    const Id mantissa =
        u(Op::OpBitwiseOr, u(Op::OpBitwiseAnd, magnitude, c(0x7fffff)), c(0x800000));
    const Id shift = extended(GLSLstd450UMin, u32_, {u(Op::OpISub, c(126), exponent), c(31)});
    const Id denormal = round_shift(mantissa, shift);
    Id half = pick(test(Op::OpULessThan, magnitude, 0x38800000), denormal, normal);
    half = pick(test(Op::OpUGreaterThanEqual, magnitude, 0x477ff000), c(0x7c00), half);
    half = pick(test(Op::OpUGreaterThan, magnitude, 0x7f800000), c(0x7e00), half);
    return u(Op::OpBitwiseOr, sign, half);
}

FormatEmitter::Words FormatEmitter::encode(unsigned dfmt, unsigned nf, const Words& value) {
    const FormatLayout& layout = *format_layout(dfmt);
    Words out;
    out.fill(b_.constant_u32(0));
    for (unsigned i = 0; i < layout.components; ++i) {
        const FormatField& f = layout.field[i];
        const Id bits = encode_field(nf, f.width, value[i]);
        out[f.word] = b_.op(Op::OpBitwiseOr, u32_,
                            {out[f.word], f.offset == 0 ? bits
                                                        : b_.op(Op::OpShiftLeftLogical, u32_,
                                                                {bits, b_.constant_u32(f.offset)})});
    }
    return out;
}

}  // namespace bb::gcn
