// VOP1: decoding and disassembly of words encoded by hand from the Sea Islands ISA manual, and a
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
        {0x00000000},       // VOP2, not decoded yet
        {0x7c000000},       // VOPC, not decoded yet
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

TEST(GcnVop, RoundTripEveryOpcode) {
    const auto words = every_vop1_opcode();
    const auto insts = decode(words, Until::code_end);
    EXPECT_EQ(insts.size(), 65u);  // opcodes 0-26 and 32-70, without v_mov_fed_b32
    if (!have_llvm_mc()) {
        GTEST_SKIP() << "llvm-mc not found";
    }
    std::vector<std::string> lines;
    for (const Instruction& inst : insts) {
        lines.push_back(disassemble(inst));
    }
    const auto assembled = assemble(lines);
    for (std::size_t i = 0; i < insts.size(); ++i) {
        EXPECT_EQ(assembled[i].words, std::vector<std::uint32_t>{words[i]}) << lines[i];
    }
}

}  // namespace
