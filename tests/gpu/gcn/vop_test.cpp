// VOP1, VOP2 and VOPC: decoding and disassembly of words encoded by hand from the Sea Islands ISA manual, and a
// round trip of every opcode in the table through llvm-mc.

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "gpu/gcn/encode.hpp"
#include "gpu/gcn/gcn.hpp"
#include "gpu/gcn/llvm_mc.hpp"

namespace {

using namespace bb::gcn;
using namespace bb::test::gcn;

constexpr unsigned v = 256;  // operand code of v0 in src0

Instruction one(const std::vector<std::uint32_t>& words) {
    auto insts = decode(words, Until::code_end);
    EXPECT_EQ(insts.size(), 1u);
    EXPECT_EQ(insts.at(0).size, words.size());
    return insts.at(0);
}

Operand vgpr(unsigned n, std::uint8_t dwords = 1) {
    return {OperandKind::vector, dwords, static_cast<std::uint16_t>(n), 0};
}

Operand sreg(std::uint16_t c, std::uint8_t dwords = 1) {
    return {OperandKind::scalar, dwords, c, 0};
}

TEST(GcnVop, Vop1) {
    auto inst = one({vop1(3, 1, v + 254)});
    EXPECT_EQ(inst.format, Format::vop1);
    EXPECT_EQ(inst.name, "v_cvt_i32_f64_e32");
    EXPECT_EQ(inst.dst[0], vgpr(1));
    EXPECT_EQ(inst.src[0], vgpr(254, 2));

    inst = one({vop1(4, 1, 2)});  // VGPR ranges don't need alignment
    EXPECT_EQ(inst.dst[0], vgpr(1, 2));
    EXPECT_EQ(inst.src[0], sreg(2));

    inst = one({vop1(2, code::vcc, v + 1)});
    EXPECT_EQ(inst.name, "v_readfirstlane_b32");
    EXPECT_EQ(inst.dst[0], sreg(code::vcc));

    inst = one({vop1(1, 1, code::lds_direct)});
    EXPECT_EQ(inst.src[0], sreg(code::lds_direct));

    inst = one({vop1(0, 0, 0)});
    EXPECT_EQ(inst.name, "v_nop");
    EXPECT_EQ(inst.num_dst + inst.num_src, 0);
}

TEST(GcnVop, Errors) {
    const std::vector<std::vector<std::uint32_t>> bad = {
        {vop1(27, 1, v)},   // unknown VOP1 opcode
        {vop1(4, 255, v)},  // v[255:256]
        {vop1(15, 1, 3)},   // misaligned SGPR pair
        {vop1(1, 1, 209)},  // reserved operand code
        {vop1(2, 128, v)},  // a constant as destination
        {vop1(1, 1, 255)},  // literal past the end
        {vop1(15, 1, code::lds_direct)},  // LDS_DIRECT as a 64-bit source
        {sop1(3, 1, code::lds_direct)},   // LDS_DIRECT in a scalar format
        {vop2(50, 1, v, 2)},                   // unknown VOP2 opcode
        {vopc(0x89, v, 2)},                    // unknown VOPC opcode
        {vopc(0x21, v, 255)},                  // v[255:256]
        {vop2(1, 128, v + 2, 3)},              // v_readlane_b32 to a constant
        {vop2(1, 1, v + 2, 255)},              // lane select literal past the end
        {vop2(2, 1, 2, code::lds_direct)},     // LDS_DIRECT as the lane select
        {vop2(32, 1, v + 2, 3)},               // v_madmk_f32 literal past the end
        {vop2(33, 1, v + 2, 3)},               // v_madak_f32 literal past the end
    };
    for (const auto& words : bad) {
        SCOPED_TRACE(words[0]);
        EXPECT_THROW(decode(words, Until::code_end), GcnError);
    }
}

struct Case {
    std::vector<std::uint32_t> words;
    std::string text;
};

const std::vector<Case> cases = {
    {{vop1(1, 1, v + 2)}, "v_mov_b32_e32 v1, v2"},
    {{vop1(1, 1, 242)}, "v_mov_b32_e32 v1, 1.0"},
    {{vop1(1, 1, 255), 0x41200000}, "v_mov_b32_e32 v1, 0x41200000"},
    {{vop1(1, 1, 255), 0x3f800000}, "v_mov_b32_e32 v1, lit(0x3f800000)"},
    {{vop1(1, 1, 255), 0xfffffff0}, "v_mov_b32_e32 v1, lit(0xfffffff0)"},
    {{vop1(1, 1, code::exec + 1)}, "v_mov_b32_e32 v1, exec_hi"},
    {{vop1(1, 255, code::scc)}, "v_mov_b32_e32 v255, src_scc"},
    {{vop1(1, 1, code::ttmp0 + 3)}, "v_mov_b32_e32 v1, ttmp3"},
    {{vop1(1, 1, code::lds_direct)}, "v_mov_b32_e32 v1, src_lds_direct"},
    {{vop1(4, 2, code::lds_direct)}, "v_cvt_f64_i32_e32 v[2:3], src_lds_direct"},
    {{vop1(4, 254, v + 1)}, "v_cvt_f64_i32_e32 v[254:255], v1"},
    {{vop1(15, 1, 2)}, "v_cvt_f32_f64_e32 v1, s[2:3]"},
    {{vop1(15, 1, 244)}, "v_cvt_f32_f64_e32 v1, 2.0"},
    {{vop1(15, 1, 208)}, "v_cvt_f32_f64_e32 v1, -16"},
    {{vop1(15, 1, 255), 0x12345678}, "v_cvt_f32_f64_e32 v1, 0x12345678"},
    {{vop1(15, 1, 255), 0x40000000}, "v_cvt_f32_f64_e32 v1, lit(0x40000000)"},
    {{vop1(15, 1, 255), 5}, "v_cvt_f32_f64_e32 v1, lit(0x5)"},
    // The f16 source of v_cvt_f32_f16: LLVM reads a literal as 16 bits.
    {{vop1(11, 1, 242)}, "v_cvt_f32_f16_e32 v1, 1.0"},
    {{vop1(11, 1, 255), 0x3c00}, "v_cvt_f32_f16_e32 v1, lit(0x3c00)"},
    {{vop1(11, 1, 255), 0xbc00}, "v_cvt_f32_f16_e32 v1, lit(0xbc00)"},
    {{vop1(11, 1, 255), 0x4400}, "v_cvt_f32_f16_e32 v1, lit(0x4400)"},
    {{vop1(11, 1, 255), 0x3800}, "v_cvt_f32_f16_e32 v1, lit(0x3800)"},
    {{vop1(11, 1, 255), 0x40}, "v_cvt_f32_f16_e32 v1, lit(0x40)"},
    {{vop1(11, 1, 255), 0xfff0}, "v_cvt_f32_f16_e32 v1, lit(0xfff0)"},
    {{vop1(11, 1, 255), 0xffff}, "v_cvt_f32_f16_e32 v1, lit(0xffff)"},
    {{vop1(11, 1, 255), 0x41}, "v_cvt_f32_f16_e32 v1, 0x41"},
    {{vop1(11, 1, 255), 0xffef}, "v_cvt_f32_f16_e32 v1, 0xffef"},
    {{vop1(11, 1, 255), 0x3c01}, "v_cvt_f32_f16_e32 v1, 0x3c01"},
    {{vop1(10, 1, 255), 0x3c00}, "v_cvt_f16_f32_e32 v1, 0x3c00"},  // an f32 source
    {{vop1(2, code::vcc, v + 1)}, "v_readfirstlane_b32 vcc_lo, v1"},
    {{vop1(2, 1, code::lds_direct)}, "v_readfirstlane_b32 s1, src_lds_direct"},
    {{vop1(0, 0, 0)}, "v_nop"},
    {{vop1(65, 0, 0)}, "v_clrexcp"},
    // VOP2.
    {{vop2(3, 1, v + 2, 3)}, "v_add_f32_e32 v1, v2, v3"},
    {{vop2(3, 1, 242, 3)}, "v_add_f32_e32 v1, 1.0, v3"},
    {{vop2(3, 1, 255, 3), 0x41200000}, "v_add_f32_e32 v1, 0x41200000, v3"},
    {{vop2(3, 1, 255, 3), 0x3f800000}, "v_add_f32_e32 v1, lit(0x3f800000), v3"},
    {{vop2(8, 255, code::lds_direct, 255)}, "v_mul_f32_e32 v255, src_lds_direct, v255"},
    {{vop2(49, 1, 2, 3)}, "v_cvt_pk_i16_i32_e32 v1, s2, v3"},
    {{vop2(0, 1, v + 2, 3)}, "v_cndmask_b32_e32 v1, v2, v3, vcc"},
    {{vop2(37, 1, v + 2, 3)}, "v_add_i32_e32 v1, vcc, v2, v3"},
    {{vop2(38, 1, 133, 3)}, "v_sub_i32_e32 v1, vcc, 5, v3"},
    {{vop2(40, 1, v + 2, 3)}, "v_addc_u32_e32 v1, vcc, v2, v3, vcc"},
    {{vop2(41, 1, v + 2, 3)}, "v_subb_u32_e32 v1, vcc, v2, v3, vcc"},
    {{vop2(42, 1, v + 2, 3)}, "v_subbrev_u32_e32 v1, vcc, v2, v3, vcc"},
    {{vop2(1, 1, v + 2, 3)}, "v_readlane_b32 s1, v2, s3"},
    {{vop2(1, code::vcc + 1, v + 2, code::m0)}, "v_readlane_b32 vcc_hi, v2, m0"},
    {{vop2(1, 1, v + 2, 133)}, "v_readlane_b32 s1, v2, 5"},
    {{vop2(2, 1, 2, 133)}, "v_writelane_b32 v1, s2, 5"},
    {{vop2(2, 1, 133, 7)}, "v_writelane_b32 v1, 5, s7"},
    {{vop2(32, 1, v + 2, 3), 0x41200000}, "v_madmk_f32 v1, v2, 0x41200000, v3"},
    {{vop2(32, 1, v + 2, 3), 0x3f800000}, "v_madmk_f32 v1, v2, 0x3f800000, v3"},
    {{vop2(33, 1, v + 2, 3), 0x41200000}, "v_madak_f32 v1, v2, v3, 0x41200000"},
    {{vop2(33, 1, v + 2, 3), 5}, "v_madak_f32 v1, v2, v3, 0x5"},
    // VOPC.
    {{vopc(1, v + 1, 2)}, "v_cmp_lt_f32_e32 vcc, v1, v2"},
    {{vopc(0x11, 4, 2)}, "v_cmpx_lt_f32_e32 vcc, s4, v2"},
    {{vopc(0x4f, 244, 2)}, "v_cmps_tru_f32_e32 vcc, 2.0, v2"},
    {{vopc(0x5f, v + 1, 2)}, "v_cmpsx_tru_f32_e32 vcc, v1, v2"},
    {{vopc(0x22, v + 1, 4)}, "v_cmp_eq_f64_e32 vcc, v[1:2], v[4:5]"},
    {{vopc(0x22, 4, 4)}, "v_cmp_eq_f64_e32 vcc, s[4:5], v[4:5]"},
    {{vopc(0x22, 255, 4), 0x40000000}, "v_cmp_eq_f64_e32 vcc, lit(0x40000000), v[4:5]"},
    {{vopc(0x22, 255, 4), 0x12345678}, "v_cmp_eq_f64_e32 vcc, 0x12345678, v[4:5]"},
    {{vopc(0x80, v + 1, 2)}, "v_cmp_f_i32_e32 vcc, v1, v2"},
    {{vopc(0x97, v + 1, 2)}, "v_cmpx_t_i32_e32 vcc, v1, v2"},
    {{vopc(0xa5, 106, 2)}, "v_cmp_ne_i64_e32 vcc, vcc, v[2:3]"},
    {{vopc(0xc6, v + 1, 2)}, "v_cmp_ge_u32_e32 vcc, v1, v2"},
    {{vopc(0xf7, v + 1, 2)}, "v_cmpx_t_u64_e32 vcc, v[1:2], v[2:3]"},
    {{vopc(0x88, v + 1, 2)}, "v_cmp_class_f32_e32 vcc, v1, v2"},
    {{vopc(0x98, 255, 2), 0x3f800000}, "v_cmpx_class_f32_e32 vcc, lit(0x3f800000), v2"},
    {{vopc(0xa8, v + 1, 4)}, "v_cmp_class_f64_e32 vcc, v[1:2], v4"},
    {{vopc(0xb8, 4, 4)}, "v_cmpx_class_f64_e32 vcc, s[4:5], v4"},
};

std::string text(const std::vector<std::uint32_t>& words) {
    auto insts = decode(words, Until::code_end);
    EXPECT_EQ(insts.size(), 1u);
    return disassemble(insts.at(0));
}

TEST(GcnVop, Text) {
    std::vector<std::string> lines;
    for (const Case& c : cases) {
        EXPECT_EQ(text(c.words), c.text) << c.words[0];
        lines.push_back(c.text);
    }
    if (!have_llvm_mc()) {
        GTEST_SKIP() << "llvm-mc not found";
    }
    const auto assembled = assemble(lines);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        EXPECT_EQ(assembled[i].words, cases[i].words) << cases[i].text;
    }
}

// Encodings that LLVM's syntax can't express: the decoder accepts what the fields allow.
TEST(GcnVop, EncodingsLlvmRejects) {
    const std::vector<Case> rejected = {
        {{vop1(9, 1, v + 2)}, "v_mov_fed_b32_e32 v1, v2"},          // no gfx7 syntax
        {{vop1(2, code::m0, v + 1)}, "v_readfirstlane_b32 m0, v1"},  // m0 as destination
        {{vop1(2, 1, 133)}, "v_readfirstlane_b32 s1, 5"},  // a source other than a VGPR
        {{vop1(2, 1, 2)}, "v_readfirstlane_b32 s1, s2"},
        // A literal above 0xffff on an f16 source, also with lit().
        {{vop1(11, 1, 255), 0x12345678}, "v_cvt_f32_f16_e32 v1, 0x12345678"},
        {{vop1(11, 1, 255), 0x10000}, "v_cvt_f32_f16_e32 v1, 0x10000"},
        {{vop1(11, 1, 255), 0x3f800000}, "v_cvt_f32_f16_e32 v1, 0x3f800000"},
        // m0 as the destination of v_readlane_b32, and two different SGPRs or an SGPR and a literal on the constant bus.
        {{vop2(1, code::m0, v + 2, 3)}, "v_readlane_b32 m0, v2, s3"},
        {{vop2(2, 1, 2, 3)}, "v_writelane_b32 v1, s2, s3"},
        {{vop2(33, 1, 2, 3), 5}, "v_madak_f32 v1, s2, v3, 0x5"},  // an SGPR and a literal
        {{vop2(42, 1, 2, 3)}, "v_subbrev_u32_e32 v1, vcc, s2, v3, vcc"},  // an SGPR and VCC
        {{vop2(1, 1, v + 2, 255), 0x12345678}, "v_readlane_b32 s1, v2, 0x12345678"},  // literal
    };
    for (const Case& c : rejected) {
        SCOPED_TRACE(c.text);
        EXPECT_EQ(text(c.words), c.text);
        if (have_llvm_mc()) {
            EXPECT_THROW(assemble({c.text}), std::runtime_error);
        }
    }
}

// One encoding per VOP1 opcode, with v2 as vdst and v4 as src0 (s1 as destination of
// v_readfirstlane_b32), except for v_mov_fed_b32, which has no gfx7 syntax in LLVM.
std::vector<std::uint32_t> every_vop1_opcode() {
    std::vector<std::uint32_t> words;
    for (unsigned op = 0; op < 256; ++op) {
        // v_nop and v_clrexcp don't use the fields, and LLVM sets them to 0.
        const bool none = op == 0 || op == 65;
        const std::uint32_t w = vop1(op, none ? 0 : op == 2 ? 1 : 2, none ? 0 : v + 4);
        try {
            if (one({w}).name != "v_mov_fed_b32_e32") {
                words.push_back(w);
            }
        } catch (const GcnError&) {
        }
    }
    return words;
}

// One encoding per VOP2 opcode (v2, v4, v6; s1 and the lane select s3 for v_readlane_b32, the
// inline constant 5 as lane select for v_writelane_b32, which can't have a second SGPR) and per
// VOPC opcode (v4, v6), with the literal K after v_madmk_f32 and v_madak_f32.
std::vector<std::vector<std::uint32_t>> every_vop2_opcode() {
    std::vector<std::vector<std::uint32_t>> all;
    for (unsigned op = 0; op < 0x3e; ++op) {  // 0x3e and 0x3f are VOPC and VOP1
        std::vector<std::uint32_t> w = {op == 1   ? vop2(op, 1, v + 4, 3)
                                        : op == 2 ? vop2(op, 2, 2, 133)
                                                  : vop2(op, 2, v + 4, 6)};
        if (op == 32 || op == 33) {
            w.push_back(0x41200000);
        }
        try {
            one(w);
            all.push_back(w);
        } catch (const GcnError&) {
        }
    }
    return all;
}

std::vector<std::vector<std::uint32_t>> every_vopc_opcode() {
    std::vector<std::vector<std::uint32_t>> all;
    for (unsigned op = 0; op < 256; ++op) {
        try {
            one({vopc(op, v + 4, 6)});
            all.push_back({vopc(op, v + 4, 6)});
        } catch (const GcnError&) {
        }
    }
    return all;
}

void round_trip(const std::vector<std::vector<std::uint32_t>>& all) {
    std::vector<std::string> lines;
    for (const auto& words : all) {
        lines.push_back(text(words));
    }
    const auto assembled = assemble(lines);
    for (std::size_t i = 0; i < all.size(); ++i) {
        EXPECT_EQ(assembled[i].words, all[i]) << lines[i];
    }
}

TEST(GcnVop, RoundTripEveryOpcode) {
    const auto words = every_vop1_opcode();
    const auto insts = decode(words, Until::code_end);
    EXPECT_EQ(insts.size(), 65u);  // opcodes 0-26 and 32-70, without v_mov_fed_b32
    EXPECT_EQ(every_vop2_opcode().size(), 50u);
    EXPECT_EQ(every_vopc_opcode().size(), 196u);
    if (!have_llvm_mc()) {
        GTEST_SKIP() << "llvm-mc not found";
    }
    std::vector<std::vector<std::uint32_t>> vop1_words;
    for (std::uint32_t w : words) {
        vop1_words.push_back({w});
    }
    round_trip(vop1_words);
    round_trip(every_vop2_opcode());
    round_trip(every_vopc_opcode());
}

}  // namespace
