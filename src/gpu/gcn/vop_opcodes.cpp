// Opcode table of VOP1, from the opcode lists of the Sea Islands ISA manual
// (chapter "Microcode Formats"). Regular families are built in loops.

#include <array>
#include <string>

#include "gpu/gcn/vop.hpp"

namespace bb::gcn::detail {

namespace {

using Table = std::vector<VopInfo>;

// LLVM prints the 32-bit encoding with an _e32 suffix, except for v_nop, v_clrexcp and
// v_readfirstlane_b32.
void set(Table& t, unsigned op, const std::string& name, std::uint8_t dst, std::uint8_t src0,
         std::uint8_t src1 = 0, Vop kind = Vop::plain) {
    if (t.size() <= op) {
        t.resize(op + 1);
    }
    const bool suffix = kind != Vop::none && kind != Vop::sdst;
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

}  // namespace

const std::vector<VopInfo>& vop_opcodes(Format format) {
    static const Table vop1 = make_vop1();
    static const Table none;
    switch (format) {
    case Format::vop1:
        return vop1;
    default:
        return none;
    }
}

}  // namespace bb::gcn::detail
