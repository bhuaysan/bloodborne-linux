// Opcode tables of VOP1, VOP2, VOPC and VOP3, from the opcode lists of the Sea Islands ISA manual
// (chapter "Microcode Formats"). Regular families are built in loops.

#include <algorithm>
#include <array>
#include <string>

#include "gpu/gcn/vop.hpp"

namespace bb::gcn::detail {

namespace {

using Table = std::vector<VopInfo>;

// LLVM prints the 32-bit encoding with an _e32 suffix, except for v_nop, v_clrexcp,
// v_readfirstlane_b32, v_readlane_b32, v_writelane_b32, v_madmk_f32 and v_madak_f32.
void set(Table& t, unsigned op, const std::string& name, std::uint8_t dst, std::uint8_t src0,
         std::uint8_t src1 = 0, Vop kind = Vop::plain) {
    if (t.size() <= op) {
        t.resize(op + 1);
    }
    const bool suffix = kind != Vop::none && kind != Vop::sdst && kind != Vop::readlane &&
                        kind != Vop::writelane && kind != Vop::madmk && kind != Vop::madak;
    t[op] = {"v_" + name + (suffix ? "_e32" : ""), dst, src0, src1, kind};
}

// Consecutive VOP1 opcodes from `first` with 32-bit operands.
template <std::size_t N>
void run(Table& t, unsigned first, const std::array<const char*, N>& names) {
    for (unsigned i = 0; i < N; ++i) {
        set(t, first + i, names[i], 1, 1);
    }
}

Table make_vop1() {
    Table t;
    set(t, 0, "nop", 0, 0, 0, Vop::none);
    set(t, 1, "mov_b32", 1, 1);
    set(t, 2, "readfirstlane_b32", 1, 1, 0, Vop::sdst);
    set(t, 3, "cvt_i32_f64", 1, 2);
    set(t, 4, "cvt_f64_i32", 2, 1);
    run<10>(t, 5, {"cvt_f32_i32", "cvt_f32_u32", "cvt_u32_f32", "cvt_i32_f32", "mov_fed_b32",
                   "cvt_f16_f32", "cvt_f32_f16", "cvt_rpi_i32_f32", "cvt_flr_i32_f32",
                   "cvt_off_f32_i4"});
    t[11].f16_src0 = true;  // v_cvt_f32_f16
    set(t, 15, "cvt_f32_f64", 1, 2);
    set(t, 16, "cvt_f64_f32", 2, 1);
    run<4>(t, 17, {"cvt_f32_ubyte0", "cvt_f32_ubyte1", "cvt_f32_ubyte2", "cvt_f32_ubyte3"});
    set(t, 21, "cvt_u32_f64", 1, 2);
    set(t, 22, "cvt_f64_u32", 2, 1);
    constexpr std::array<const char*, 4> rounding = {"trunc", "ceil", "rndne", "floor"};
    for (unsigned i = 0; i < rounding.size(); ++i) {
        set(t, 23 + i, std::string(rounding[i]) + "_f64", 2, 2);  // CI
    }
    run<15>(t, 32, {"fract_f32", "trunc_f32", "ceil_f32", "rndne_f32", "floor_f32", "exp_f32",
                    "log_clamp_f32", "log_f32", "rcp_clamp_f32", "rcp_legacy_f32", "rcp_f32",
                    "rcp_iflag_f32", "rsq_clamp_f32", "rsq_legacy_f32", "rsq_f32"});
    constexpr std::array<const char*, 4> f64 = {"rcp_f64", "rcp_clamp_f64", "rsq_f64",
                                                "rsq_clamp_f64"};
    for (unsigned i = 0; i < f64.size(); ++i) {
        set(t, 47 + i, f64[i], 2, 2);
    }
    set(t, 51, "sqrt_f32", 1, 1);
    set(t, 52, "sqrt_f64", 2, 2);
    run<7>(t, 53, {"sin_f32", "cos_f32", "not_b32", "bfrev_b32", "ffbh_u32", "ffbl_b32",
                   "ffbh_i32"});
    set(t, 60, "frexp_exp_i32_f64", 1, 2);
    set(t, 61, "frexp_mant_f64", 2, 2);
    set(t, 62, "fract_f64", 2, 2);
    set(t, 63, "frexp_exp_i32_f32", 1, 1);
    set(t, 64, "frexp_mant_f32", 1, 1);
    set(t, 65, "clrexcp", 0, 0, 0, Vop::none);
    run<5>(t, 66, {"movreld_b32", "movrels_b32", "movrelsd_b32", "log_legacy_f32",
                   "exp_legacy_f32"});  // the legacy log and exp are CI
    return t;
}

// Consecutive VOP2 opcodes from `first` with 32-bit operands.
template <std::size_t N>
void binary(Table& t, unsigned first, const std::array<const char*, N>& names) {
    for (unsigned i = 0; i < N; ++i) {
        set(t, first + i, names[i], 1, 1, 1, Vop::binary);
    }
}

Table make_vop2() {
    Table t;
    set(t, 0, "cndmask_b32", 1, 1, 1, Vop::cndmask);
    set(t, 1, "readlane_b32", 1, 1, 1, Vop::readlane);
    set(t, 2, "writelane_b32", 1, 1, 1, Vop::writelane);
    binary<28>(t, 3, {"add_f32", "sub_f32", "subrev_f32", "mac_legacy_f32", "mul_legacy_f32",
                      "mul_f32", "mul_i32_i24", "mul_hi_i32_i24", "mul_u32_u24",
                      "mul_hi_u32_u24", "min_legacy_f32", "max_legacy_f32", "min_f32", "max_f32",
                      "min_i32", "max_i32", "min_u32", "max_u32", "lshr_b32", "lshrrev_b32",
                      "ashr_i32", "ashrrev_i32", "lshl_b32", "lshlrev_b32", "and_b32", "or_b32",
                      "xor_b32", "bfm_b32"});
    binary<1>(t, 31, {"mac_f32"});
    set(t, 32, "madmk_f32", 1, 1, 1, Vop::madmk);
    set(t, 33, "madak_f32", 1, 1, 1, Vop::madak);
    binary<3>(t, 34, {"bcnt_u32_b32", "mbcnt_lo_u32_b32", "mbcnt_hi_u32_b32"});
    constexpr std::array<const char*, 3> carry_out = {"add_i32", "sub_i32", "subrev_i32"};
    constexpr std::array<const char*, 3> carry = {"addc_u32", "subb_u32", "subbrev_u32"};
    for (unsigned i = 0; i < 3; ++i) {
        set(t, 37 + i, carry_out[i], 1, 1, 1, Vop::carry_out);
        set(t, 40 + i, carry[i], 1, 1, 1, Vop::carry);
    }
    binary<7>(t, 43, {"ldexp_f32", "cvt_pkaccum_u8_f32", "cvt_pknorm_i16_f32",
                      "cvt_pknorm_u16_f32", "cvt_pkrtz_f16_f32", "cvt_pk_u16_u32",
                      "cvt_pk_i16_i32"});
    return t;
}

// VOPC: the float compares are at the base of their family plus 0x20 for f64 plus the condition,
// the integer compares at 0x80 plus 0x40 for unsigned, 0x20 for 64-bit and 0x10 for cmpx plus
// the condition, and the class compares at 0x88 plus 0x20 for f64 and 0x10 for cmpx.
Table make_vopc() {
    Table t;
    auto add = [&](unsigned op, const std::string& name, unsigned dwords, unsigned mask_dwords) {
        set(t, op, name, 0, static_cast<std::uint8_t>(dwords),
            static_cast<std::uint8_t>(mask_dwords), Vop::compare);
    };
    constexpr std::array<const char*, 16> fcond = {"f",   "lt",  "eq",  "le",  "gt",  "lg",
                                                   "ge",  "o",   "u",   "nge", "nlg", "ngt",
                                                   "nle", "neq", "nlt", "tru"};
    constexpr std::array<const char*, 8> icond = {"f", "lt", "eq", "le", "gt", "ne", "ge", "t"};
    constexpr std::array<const char*, 4> fbase = {"cmp", "cmpx", "cmps", "cmpsx"};
    constexpr std::array<unsigned, 4> fbase_op = {0x00, 0x10, 0x40, 0x50};
    for (unsigned b = 0; b < 4; ++b) {
        for (unsigned wide = 0; wide < 2; ++wide) {
            for (unsigned c = 0; c < 16; ++c) {
                add(fbase_op[b] + wide * 0x20 + c,
                    std::string(fbase[b]) + "_" + fcond[c] + (wide ? "_f64" : "_f32"),
                    1 + wide, 1 + wide);
            }
        }
    }
    for (unsigned x = 0; x < 2; ++x) {      // cmp, cmpx
        for (unsigned u = 0; u < 2; ++u) {  // signed, unsigned
            for (unsigned wide = 0; wide < 2; ++wide) {
                for (unsigned c = 0; c < 8; ++c) {
                    add(0x80 + u * 0x40 + wide * 0x20 + x * 0x10 + c,
                        std::string(x ? "cmpx_" : "cmp_") + icond[c] + (u ? "_u" : "_i") +
                            (wide ? "64" : "32"),
                        1 + wide, 1 + wide);
                }
            }
        }
    }
    for (unsigned x = 0; x < 2; ++x) {
        for (unsigned wide = 0; wide < 2; ++wide) {
            add(0x88 + wide * 0x20 + x * 0x10,
                std::string(x ? "cmpx" : "cmp") + "_class_" + (wide ? "f64" : "f32"), 1 + wide,
                1);
        }
    }
    return t;
}

// VOP3: the _e64 forms of the other formats (VOPC at 0, VOP2 at 0x100, VOP1 at 0x180, without the
// two opcodes whose literal K has no VOP3 form) and the VOP3-only opcodes at 0x140 to 0x177.
void only(Table& t, unsigned op, const std::string& name, Vop kind, std::uint8_t dst,
          std::uint8_t src0, std::uint8_t src1, std::uint8_t src2 = 0) {
    t.resize(std::max<std::size_t>(t.size(), op + 1));
    t[op] = {"v_" + name, dst, src0, src1, kind, false, src2};
}

// Consecutive VOP3-only opcodes from `first` with three 32-bit sources.
template <std::size_t N>
void ternary(Table& t, unsigned first, const std::array<const char*, N>& names) {
    for (unsigned i = 0; i < N; ++i) {
        only(t, first + i, names[i], Vop::ternary, 1, 1, 1, 1);
    }
}

Table make_vop3(const Table& vop1, const Table& vop2, const Table& vopc) {
    Table t;
    auto derive = [&](unsigned base, const Table& from) {
        for (unsigned op = 0; op < from.size(); ++op) {
            VopInfo info = from[op];
            if (info.name.empty() || info.kind == Vop::madmk || info.kind == Vop::madak) {
                continue;
            }
            if (info.name.ends_with("_e32")) {
                info.name.resize(info.name.size() - 4);
            }
            info.name += "_e64";
            t.resize(std::max<std::size_t>(t.size(), base + op + 1));
            t[base + op] = info;
        }
    };
    derive(0, vopc);
    derive(0x100, vop2);
    derive(0x180, vop1);
    ternary<12>(t, 0x140, {"mad_legacy_f32", "mad_f32", "mad_i32_i24", "mad_u32_u24",
                           "cubeid_f32", "cubesc_f32", "cubetc_f32", "cubema_f32", "bfe_u32",
                           "bfe_i32", "bfi_b32", "fma_f32"});
    ternary<19>(t, 0x14d, {"lerp_u8", "alignbit_b32", "alignbyte_b32", "mullit_f32", "min3_f32",
                           "min3_i32", "min3_u32", "max3_f32", "max3_i32", "max3_u32",
                           "med3_f32", "med3_i32", "med3_u32", "sad_u8", "sad_hi_u8", "sad_u16",
                           "sad_u32", "cvt_pk_u8_f32", "div_fixup_f32"});
    ternary<1>(t, 0x16f, {"div_fmas_f32"});
    ternary<1>(t, 0x171, {"msad_u8"});
    only(t, 0x14c, "fma_f64", Vop::ternary, 2, 2, 2, 2);
    only(t, 0x160, "div_fixup_f64", Vop::ternary, 2, 2, 2, 2);
    only(t, 0x170, "div_fmas_f64", Vop::ternary, 2, 2, 2, 2);
    constexpr std::array<const char*, 3> shifts = {"lshl_b64", "lshr_b64", "ashr_i64"};
    constexpr std::array<const char*, 4> f64 = {"add_f64", "mul_f64", "min_f64", "max_f64"};
    constexpr std::array<const char*, 4> mul = {"mul_lo_u32", "mul_hi_u32", "mul_lo_i32",
                                                "mul_hi_i32"};
    for (unsigned i = 0; i < 4; ++i) {
        if (i < 3) {
            only(t, 0x161 + i, shifts[i], Vop::binary, 2, 2, 1);
        }
        only(t, 0x164 + i, f64[i], Vop::binary, 2, 2, 2);
        only(t, 0x169 + i, mul[i], Vop::binary, 1, 1, 1);
    }
    only(t, 0x168, "ldexp_f64", Vop::binary, 2, 2, 1);
    only(t, 0x174, "trig_preop_f64", Vop::binary, 2, 2, 1);
    only(t, 0x16d, "div_scale_f32", Vop::ternary_sdst, 1, 1, 1, 1);
    only(t, 0x16e, "div_scale_f64", Vop::ternary_sdst, 2, 2, 2, 2);
    only(t, 0x172, "qsad_pk_u16_u8", Vop::ternary, 2, 2, 1, 2);
    only(t, 0x173, "mqsad_pk_u16_u8", Vop::ternary, 2, 2, 1, 2);
    only(t, 0x175, "mqsad_u32_u8", Vop::ternary, 4, 2, 1, 4);  // CI
    only(t, 0x176, "mad_u64_u32", Vop::ternary_sdst, 2, 1, 1, 2);  // CI
    only(t, 0x177, "mad_i64_i32", Vop::ternary_sdst, 2, 1, 1, 2);  // CI
    return t;
}

}  // namespace

const std::vector<VopInfo>& vop_opcodes(Format format) {
    static const Table vop1 = make_vop1();
    static const Table vop2 = make_vop2();
    static const Table vopc = make_vopc();
    static const Table vop3 = make_vop3(vop1, vop2, vopc);
    static const Table none;
    switch (format) {
    case Format::vop1:
        return vop1;
    case Format::vop2:
        return vop2;
    case Format::vopc:
        return vopc;
    case Format::vop3:
        return vop3;
    default:
        return none;
    }
}

}  // namespace bb::gcn::detail
