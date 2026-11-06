// VOP3: decoding and disassembly of words encoded by hand from the Sea Islands ISA manual, and a
// round trip of every opcode in the VOP3 table through llvm-mc.

#include <algorithm>
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

constexpr unsigned v = 256;  // operand code of v0 in a source field

using Words = std::vector<std::uint32_t>;

Instruction one(const Words& words) {
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

std::string text(const Words& words) {
    auto insts = decode(words, Until::code_end);
    EXPECT_EQ(insts.size(), 1u);
    return disassemble(insts.at(0));
}

TEST(GcnVop3, Operands) {
    // VOP3b: v_add_i32_e64 v1, s[2:3], v2, 5.
    auto inst = one(vop3(0x125, 1, v + 2, 133, 0, 2 << 8));
    EXPECT_EQ(inst.name, "v_add_i32_e64");
    ASSERT_EQ(inst.num_dst, 2);
    ASSERT_EQ(inst.num_src, 2);
    EXPECT_EQ(inst.dst[1], sreg(2, 2));
    EXPECT_EQ(inst.src[1].kind, OperandKind::constant);

    inst = one(vop3(0x01, code::vcc, v + 1, v + 2));  // v_cmp_lt_f32_e64 vcc, v1, v2
    ASSERT_EQ(inst.num_dst, 1);
    EXPECT_EQ(inst.dst[0], sreg(code::vcc, 2));

    inst = one(vop3(0x100, 1, v + 2, v + 3, 4));  // v_cndmask_b32_e64: the mask is s[4:5]
    ASSERT_EQ(inst.num_src, 3);
    EXPECT_EQ(inst.src[2], sreg(4, 2));

    inst = one(vop3(0x176, 2, v + 3, v + 4, v + 6, 8 << 8));  // v_mad_u64_u32 v[2:3], s[8:9], ...
    EXPECT_EQ(inst.dst[0], vgpr(2, 2));
    EXPECT_EQ(inst.dst[1], sreg(8, 2));
    EXPECT_EQ(inst.src[2], vgpr(6, 2));

    inst = one(vop3(0x175, 4, v + 8, v + 12, v + 16));
    EXPECT_EQ(inst.dst[0], vgpr(4, 4));
    EXPECT_EQ(inst.src[0], vgpr(8, 2));
    EXPECT_EQ(inst.src[1], vgpr(12));
    EXPECT_EQ(inst.src[2], vgpr(16, 4));

    inst = one(vop3(0x180, 0, 0));  // v_nop_e64: the other fields are unused
    EXPECT_EQ(inst.num_dst + inst.num_src, 0);
    inst = one(vop3(0x180, 3, v + 1, 4, 5, 0x700, 0xf8000000));
    EXPECT_EQ(inst.num_dst + inst.num_src, 0);
}

TEST(GcnVop3, Errors) {
    const std::vector<Words> bad = {
        vop3(0x178, 1, v),                       // unknown opcode
        vop3(0x132, 1, v, v),                    // VOP2 opcode 50
        vop3(0x120, 1, v, v),                    // v_madmk_f32 has no VOP3 form
        vop3(0x103, 1, 255, v),                  // a literal
        vop3(0x103, 1, v, 254),                  // LDS_DIRECT as src1
        vop3(0x183, 1, 254),                     // and as the 64-bit src0 of v_cvt_i32_f64
        vop3(0x103, 1, v, v, 0, 1 << 13),        // reserved bit 13
        vop3(0x180, 0, 0, 0, 0, 1 << 13),        // v_nop_e64 rejects reserved bits too
        vop3(0x103, 1, v, v, 0, 1 << 8),         // abs, clamp, omod and neg are not decoded yet
        vop3(0x103, 1, v, v, 0, 1 << 11),
        vop3(0x103, 1, v, v, 0, 0, 1u << 27),
        vop3(0x103, 1, v, v, 0, 0, 1u << 29),
        vop3(0x125, 1, v, v, 0, 2 << 8 | 1 << 15),  // VOP3b: reserved bit 15
        vop3(0x125, 1, v, v, 0, 3 << 8),         // misaligned SGPR pair
        vop3(0x01, 128, v, v),                   // a constant as the destination of a compare
        vop3(0x100, 1, v, v, v),                 // a VGPR as the mask
        vop3(0x101, 1, v, v),                    // a VGPR as the lane select of v_readlane_b32
        {vop3(0x103, 1, v, v)[0]},               // truncated
    };
    for (const auto& words : bad) {
        SCOPED_TRACE(words[0]);
        EXPECT_THROW(decode(words, Until::code_end), GcnError);
    }
}

struct Case {
    Words words;
    std::string text;
};

const std::vector<Case> cases = {
    {vop3(0x103, 1, v + 2, v + 3), "v_add_f32_e64 v1, v2, v3"},
    {vop3(0x103, 1, 243, v + 3), "v_add_f32_e64 v1, -1.0, v3"},
    {vop3(0x103, 1, code::exec, v + 3), "v_add_f32_e64 v1, exec_lo, v3"},
    {vop3(0x103, 255, code::lds_direct, v + 255), "v_add_f32_e64 v255, src_lds_direct, v255"},
    {vop3(0x181, 1, 133), "v_mov_b32_e64 v1, 5"},
    {vop3(0x181, 1, code::lds_direct), "v_mov_b32_e64 v1, src_lds_direct"},
    {vop3(0x183, 1, 2), "v_cvt_i32_f64_e64 v1, s[2:3]"},
    {vop3(0x183, 1, 244), "v_cvt_i32_f64_e64 v1, 2.0"},
    {vop3(0x180, 0, 0), "v_nop_e64"},
    {vop3(0x1c1, 0, 0), "v_clrexcp_e64"},
    {vop3(0x125, 1, v + 2, v + 3, 0, 2 << 8), "v_add_i32_e64 v1, s[2:3], v2, v3"},
    {vop3(0x127, 1, 133, v + 3, 0, code::vcc << 8), "v_subrev_i32_e64 v1, vcc, 5, v3"},
    {vop3(0x128, 1, v + 2, v + 3, 4, 2 << 8), "v_addc_u32_e64 v1, s[2:3], v2, v3, s[4:5]"},
    {vop3(0x12a, 1, v + 2, v + 3, code::vcc, 2 << 8),
     "v_subbrev_u32_e64 v1, s[2:3], v2, v3, vcc"},
    {vop3(0x100, 1, v + 2, v + 3, 4), "v_cndmask_b32_e64 v1, v2, v3, s[4:5]"},
    {vop3(0x01, 2, v + 1, v + 2), "v_cmp_lt_f32_e64 s[2:3], v1, v2"},
    {vop3(0x21, 2, v + 1, v + 4), "v_cmp_lt_f64_e64 s[2:3], v[1:2], v[4:5]"},
    {vop3(0x88, 2, v + 1, 133), "v_cmp_class_f32_e64 s[2:3], v1, 5"},
    {vop3(0xb8, 2, 4, v + 4), "v_cmpx_class_f64_e64 s[2:3], s[4:5], v4"},
    {vop3(0xc6, 2, v + 1, v + 2), "v_cmp_ge_u32_e64 s[2:3], v1, v2"},
    {vop3(0x143, 1, v + 2, v + 3, 133), "v_mad_u32_u24 v1, v2, v3, 5"},
    {vop3(0x161, 2, v + 4, 133), "v_lshl_b64 v[2:3], v[4:5], 5"},
    {vop3(0x16d, 1, v + 2, v + 3, v + 4, code::vcc << 8), "v_div_scale_f32 v1, vcc, v2, v3, v4"},
    {vop3(0x16e, 2, v + 4, v + 6, v + 8, 10 << 8),
     "v_div_scale_f64 v[2:3], s[10:11], v[4:5], v[6:7], v[8:9]"},
    {vop3(0x16f, 1, v + 2, v + 3, v + 4), "v_div_fmas_f32 v1, v2, v3, v4"},
    {vop3(0x177, 2, v + 3, v + 4, v + 6, 8 << 8), "v_mad_i64_i32 v[2:3], s[8:9], v3, v4, v[6:7]"},
    {vop3(0x175, 4, 8, v + 12, v + 16), "v_mqsad_u32_u8 v[4:7], s[8:9], v12, v[16:19]"},
};

TEST(GcnVop3, Text) {
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
TEST(GcnVop3, EncodingsLlvmRejects) {
    const std::vector<Case> rejected = {
        // The VOP3 forms of the lane instructions and v_mov_fed_b32.
        {vop3(0x101, 1, v + 2, 3), "v_readlane_b32_e64 s1, v2, s3"},
        {vop3(0x102, 1, 2, 3), "v_writelane_b32_e64 v1, s2, s3"},
        {vop3(0x182, 1, v + 2), "v_readfirstlane_b32_e64 s1, v2"},
        {vop3(0x189, 1, v + 2), "v_mov_fed_b32_e64 v1, v2"},
    };
    for (const Case& c : rejected) {
        SCOPED_TRACE(c.text);
        EXPECT_EQ(text(c.words), c.text);
        if (have_llvm_mc()) {
            EXPECT_THROW(assemble({c.text}), std::runtime_error);
        }
    }
}

// One encoding per VOP3 opcode: v4 as the destination (s[4:5] for compares), v8 and v12 as the
// first two sources and v16 as the third, or s[8:9] for the mask or carry of v_cndmask_b32 and
// the carry instructions, with 0 in the unused source fields (LLVM's choice) and in the other
// fields of v_nop and v_clrexcp. LLVM has no syntax for `no_syntax`, and the lane selects of
// v_readlane_b32 and v_writelane_b32 can't be VGPRs.
TEST(GcnVop3, RoundTripEveryOpcode) {
    const std::vector<std::string> no_syntax = {"v_readfirstlane_b32_e64", "v_mov_fed_b32_e64"};
    std::vector<Words> all;
    std::vector<std::string> lines;
    for (unsigned op = 0; op < 0x200; ++op) {
        Words w;
        for (unsigned src2 : {v + 16, 8u}) {
            w = vop3(op, 4, v + 8, v + 12, src2);
            try {
                const Instruction inst = one(w);
                if (inst.num_dst + inst.num_src == 0) {
                    w = vop3(op, 0, 0);
                }
                // LLVM encodes the source fields that an instruction doesn't use as 0.
                for (unsigned shift : {9u, 18u}) {
                    Words zeroed = w;
                    zeroed[1] &= ~(0x1ffu << shift);
                    try {
                        if (text(zeroed) == disassemble(inst)) {
                            w = zeroed;
                        }
                    } catch (const GcnError&) {
                    }
                }
                if (std::find(no_syntax.begin(), no_syntax.end(), inst.name) == no_syntax.end()) {
                    all.push_back(w);
                    lines.push_back(disassemble(inst));
                }
                break;
            } catch (const GcnError&) {
            }
        }
    }
    // VOPC 196, VOP2 48 without v_madmk_f32, v_madak_f32, v_readlane_b32 and v_writelane_b32,
    // VOP1 64 without v_readfirstlane_b32 and v_mov_fed_b32, and 56 VOP3-only opcodes.
    EXPECT_EQ(all.size(), 196u + 46 + 64 + 56);
    if (!have_llvm_mc()) {
        GTEST_SKIP() << "llvm-mc not found";
    }
    const auto assembled = assemble(lines);
    for (std::size_t i = 0; i < all.size(); ++i) {
        EXPECT_EQ(assembled[i].words, all[i]) << lines[i];
    }
}

}  // namespace
