// Decoder tests with words encoded by hand from the Sea Islands ISA manual.

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "gpu/gcn/encode.hpp"
#include "gpu/gcn/gcn.hpp"

namespace {

using namespace bb::gcn;
using namespace bb::test::gcn;

// The single instruction in `words`, which must use all of them.
Instruction one(const std::vector<std::uint32_t>& words) {
    auto insts = decode(words, Until::code_end);
    EXPECT_EQ(insts.size(), 1u);
    EXPECT_EQ(insts.at(0).size, words.size());
    return insts.at(0);
}

Operand reg(std::uint16_t c, std::uint8_t dwords = 1) {
    return {OperandKind::scalar, dwords, c, 0};
}

Operand constant(std::uint16_t c, std::uint32_t value, std::uint8_t dwords = 1) {
    return {OperandKind::constant, dwords, c, value};
}

Operand literal(std::uint32_t value, std::uint8_t dwords = 1) {
    return {OperandKind::literal, dwords, code::literal, value};
}

Operand imm(OperandKind kind, std::uint32_t value) {
    return {kind, 0, 0, value};
}

// The GcnError that decoding `words` throws.
GcnError error(const std::vector<std::uint32_t>& words, Until until = Until::code_end) {
    try {
        decode(words, until);
    } catch (const GcnError& e) {
        return e;
    }
    ADD_FAILURE() << "no GcnError";
    return GcnError("none", 0, 0);
}

TEST(GcnDecode, Sop2) {
    auto inst = one({sop2(0, 0, 1, 2)});
    EXPECT_EQ(inst.format, Format::sop2);
    EXPECT_EQ(inst.opcode, 0);
    EXPECT_EQ(inst.name, "s_add_u32");
    ASSERT_EQ(inst.num_dst, 1);
    ASSERT_EQ(inst.num_src, 2);
    EXPECT_EQ(inst.dst[0], reg(0));
    EXPECT_EQ(inst.src[0], reg(1));
    EXPECT_EQ(inst.src[1], reg(2));

    // 64-bit shifts take a 32-bit shift amount.
    inst = one({sop2(31, 2, 4, 6)});
    EXPECT_EQ(inst.name, "s_lshl_b64");
    EXPECT_EQ(inst.dst[0], reg(2, 2));
    EXPECT_EQ(inst.src[0], reg(4, 2));
    EXPECT_EQ(inst.src[1], reg(6));

    inst = one({sop2(43, 0, 4, 8)});
    EXPECT_EQ(inst.name, "s_cbranch_g_fork");
    EXPECT_EQ(inst.num_dst, 0);
    EXPECT_EQ(inst.src[1], reg(8, 2));

    // Both sources read the same literal.
    inst = one({sop2(2, code::m0, 255, 255), 0xfffffff0});
    EXPECT_EQ(inst.name, "s_add_i32");
    EXPECT_EQ(inst.dst[0], reg(code::m0));
    EXPECT_EQ(inst.src[0], literal(0xfffffff0));
    EXPECT_EQ(inst.src[1], literal(0xfffffff0));
}

TEST(GcnDecode, Sop1) {
    auto inst = one({sop1(4, code::vcc, code::exec)});
    EXPECT_EQ(inst.format, Format::sop1);
    EXPECT_EQ(inst.name, "s_mov_b64");
    EXPECT_EQ(inst.dst[0], reg(code::vcc, 2));
    EXPECT_EQ(inst.src[0], reg(code::exec, 2));

    // Inline constants.
    EXPECT_EQ(one({sop1(3, 0, 128)}).src[0], constant(128, 0));
    EXPECT_EQ(one({sop1(3, 0, 192)}).src[0], constant(192, 64));
    EXPECT_EQ(one({sop1(3, 0, 193)}).src[0], constant(193, 0xffffffff));
    EXPECT_EQ(one({sop1(3, 0, 208)}).src[0], constant(208, 0xfffffff0));
    EXPECT_EQ(one({sop1(3, 0, 240)}).src[0], constant(240, 0x3f000000));     // 0.5
    // On a 64-bit operand a float constant is the f64 of the number: 0xc010000000000000 for -4.0.
    EXPECT_EQ(one({sop1(4, 0, 247)}).src[0], constant(247, 0xc0100000, 2));  // -4.0
    EXPECT_EQ(one({sop1(4, 0, 242)}).src[0], constant(242, 0x3ff00000, 2));  // 1.0
    EXPECT_EQ(one({sop1(4, 0, 240)}).src[0], constant(240, 0x3fe00000, 2));  // 0.5

    inst = one({sop1(31, 0, 0)});
    EXPECT_EQ(inst.name, "s_getpc_b64");
    EXPECT_EQ(inst.num_src, 0);
    inst = one({sop1(32, 0, 4)});
    EXPECT_EQ(inst.name, "s_setpc_b64");
    EXPECT_EQ(inst.num_dst, 0);
    EXPECT_EQ(inst.src[0], reg(4, 2));
    inst = one({sop1(16, 0, 2)});
    EXPECT_EQ(inst.name, "s_bcnt1_i32_b64");
    EXPECT_EQ(inst.dst[0], reg(0));
    EXPECT_EQ(inst.src[0], reg(2, 2));
}

TEST(GcnDecode, Sopc) {
    auto inst = one({sopc(6, 0, 242)});
    EXPECT_EQ(inst.format, Format::sopc);
    EXPECT_EQ(inst.name, "s_cmp_eq_u32");
    EXPECT_EQ(inst.num_dst, 0);
    EXPECT_EQ(inst.src[0], reg(0));
    EXPECT_EQ(inst.src[1], constant(242, 0x3f800000));
    inst = one({sopc(15, 4, 9)});
    EXPECT_EQ(inst.name, "s_bitcmp1_b64");
    EXPECT_EQ(inst.src[0], reg(4, 2));
}

TEST(GcnDecode, Sopk) {
    auto inst = one({sopk(0, 0, 0x1234)});
    EXPECT_EQ(inst.format, Format::sopk);
    EXPECT_EQ(inst.name, "s_movk_i32");
    EXPECT_EQ(inst.dst[0], reg(0));
    EXPECT_EQ(inst.src[0], imm(OperandKind::simm, 0x1234));

    // s_cmpk_* compare the register in the sdst field.
    inst = one({sopk(10, 5, 0x8000)});
    EXPECT_EQ(inst.name, "s_cmpk_lg_u32");
    EXPECT_EQ(inst.num_dst, 0);
    EXPECT_EQ(inst.src[0], reg(5));
    EXPECT_EQ(inst.src[1], imm(OperandKind::simm, 0x8000));

    inst = one({sopk(18, 0, 0xf801)});
    EXPECT_EQ(inst.name, "s_getreg_b32");
    EXPECT_EQ(inst.src[0], imm(OperandKind::hwreg, 0xf801));
    inst = one({sopk(19, 3, 0xf801)});
    EXPECT_EQ(inst.name, "s_setreg_b32");
    EXPECT_EQ(inst.num_dst, 0);
    EXPECT_EQ(inst.src[0], imm(OperandKind::hwreg, 0xf801));
    EXPECT_EQ(inst.src[1], reg(3));
    inst = one({sopk(21, 0, 0xf801), 3});
    EXPECT_EQ(inst.name, "s_setreg_imm32_b32");
    EXPECT_EQ(inst.src[1], literal(3));

    inst = one({sopk(17, 4, 0xffff)});
    EXPECT_EQ(inst.name, "s_cbranch_i_fork");
    EXPECT_EQ(inst.src[0], reg(4, 2));
    EXPECT_EQ(branch_target(inst), 0u);
}

TEST(GcnDecode, Smrd) {
    auto inst = one({smrd(2, 0, 4, true, 0x10)});
    EXPECT_EQ(inst.format, Format::smrd);
    EXPECT_EQ(inst.name, "s_load_dwordx4");
    EXPECT_EQ(inst.dst[0], reg(0, 4));
    EXPECT_EQ(inst.src[0], reg(4, 2));
    EXPECT_EQ(inst.src[1], imm(OperandKind::offset, 0x10));

    inst = one({smrd(12, 16, 8, false, 6)});
    EXPECT_EQ(inst.name, "s_buffer_load_dwordx16");
    EXPECT_EQ(inst.dst[0], reg(16, 16));
    EXPECT_EQ(inst.src[0], reg(8, 4));
    EXPECT_EQ(inst.src[1], reg(6));

    // CI: a 32-bit literal offset.
    EXPECT_EQ(one({smrd(0, 0, 4, false, 255), 0x12345}).src[1], literal(0x12345));

    inst = one({smrd(30, 2, 0, false, 0)});
    EXPECT_EQ(inst.name, "s_memtime");
    EXPECT_EQ(inst.dst[0], reg(2, 2));
    EXPECT_EQ(inst.num_src, 0);

    EXPECT_EQ(error({smrd(0, 0, 4, false, 128)}).word(), smrd(0, 0, 4, false, 128));
    // A condition bit as offset register: LLVM accepts it, the manual allows only an SGPR.
    EXPECT_EQ(error({smrd(0, 0, 4, false, code::vccz)}).word(), smrd(0, 0, 4, false, code::vccz));
    EXPECT_EQ(error({smrd(2, 2, 4, true, 0)}).word(), smrd(2, 2, 4, true, 0));  // s[2:5]

    // Special registers as base and offset register.
    inst = one({smrd(8, 0, code::ttmp0 + 4, false, code::m0)});
    EXPECT_EQ(inst.src[0], reg(code::ttmp0 + 4, 4));
    EXPECT_EQ(inst.src[1], reg(code::m0));
    EXPECT_EQ(error({smrd(8, 0, code::ttmp0 + 2, true, 0)}).word(),
              smrd(8, 0, code::ttmp0 + 2, true, 0));  // ttmp[2:5]
    EXPECT_EQ(error({smrd(3, 2, 0, true, 0)}).word(), smrd(3, 2, 0, true, 0));      // s[2:9]
    EXPECT_EQ(error({smrd(0, 0, 0, false, 255)}).word(), smrd(0, 0, 0, false, 255));  // no literal
}

TEST(GcnDecode, Sopp) {
    auto inst = one({sopp(1)});
    EXPECT_EQ(inst.format, Format::sopp);
    EXPECT_TRUE(is_endpgm(inst));
    EXPECT_EQ(inst.num_src, 0);
    EXPECT_EQ(one({sopp(0, 7)}).src[0], imm(OperandKind::count, 7));
    EXPECT_EQ(one({sopp(12, 0x007f)}).src[0], imm(OperandKind::waitcnt, 0x007f));
    EXPECT_EQ(one({sopp(16, 0x0003)}).src[0], imm(OperandKind::sendmsg, 0x0003));
    EXPECT_FALSE(is_endpgm(one({sopp(10)})));
    EXPECT_EQ(one({sopp(11, 1)}).name, "s_setkill");

    // The branch offset counts dwords from the next instruction.
    std::vector<std::uint32_t> words = {sopp(1), sopp(4, 0xfffe)};
    auto insts = decode(words, Until::code_end);
    EXPECT_EQ(insts[1].name, "s_cbranch_scc0");
    EXPECT_EQ(insts[1].src[0], imm(OperandKind::branch, 0xfffffffe));
    EXPECT_EQ(branch_target(insts[1]), 0u);
    EXPECT_EQ(branch_target(insts[0]), std::nullopt);
}

TEST(GcnDecode, Errors) {
    auto e = error({sopp(1), sopp(3)});  // 3 is reserved
    EXPECT_EQ(e.offset(), 4u);
    EXPECT_EQ(e.word(), sopp(3));
    EXPECT_NE(std::string(e.what()).find("unknown SOPP opcode 3 at offset 0x4 (word 0xbf830000)"),
              std::string::npos);

    EXPECT_EQ(error({vop2(50, 0, 0, 0)}).word(), vop2(50, 0, 0, 0));  // unknown VOP2 opcode
    EXPECT_EQ(error({sopp(2, 0xfffe)}).word(), sopp(2, 0xfffe));  // before the start
    EXPECT_NE(std::string(error({sop2(12, 0, 0, 0)}).what()).find("unknown SOP2 opcode 12"),
              std::string::npos);
}

TEST(GcnDecode, OperandErrors) {
    // A literal past the end of the code.
    EXPECT_EQ(error({sop1(3, 0, 255)}).word(), sop1(3, 0, 255));

    // Reserved operand codes, and LDS_DIRECT, which scalar instructions can't read.
    for (unsigned c : {125u, 209u, 239u, 248u, 250u, 254u}) {
        SCOPED_TRACE(c);
        EXPECT_EQ(error({sop1(3, 0, c)}).word(), sop1(3, 0, c));
    }

    // 64-bit operands: the special registers that are 32-bit, odd halves of pairs, ranges past
    // s103, misaligned SGPR ranges.
    for (unsigned c : {unsigned{code::m0}, code::vcc + 1u, 103u, 5u, 113u, 123u}) {
        SCOPED_TRACE(c);
        EXPECT_EQ(error({sop1(4, 0, c)}).word(), sop1(4, 0, c));
    }
    EXPECT_EQ(error({sop1(4, 1, 0)}).word(), sop1(4, 1, 0));  // destination s[1:2]
    EXPECT_NE(std::string(error({sop1(4, 0, 5)}).what()).find("invalid 2-dword scalar operand 5"),
              std::string::npos);
}

TEST(GcnDecode, ScalarRegisters) {
    // The last SGPR pair, the special register pairs and trap temporaries.
    for (unsigned c : {102u, unsigned{code::flat_scratch}, unsigned{code::vcc}, unsigned{code::tba},
                       unsigned{code::tma}, unsigned{code::exec}, code::ttmp0 + 10u}) {
        SCOPED_TRACE(c);
        EXPECT_EQ(one({sop1(4, 0, c)}).src[0], reg(c, 2));
    }
    // The condition bits, as 32-bit and 64-bit sources.
    for (unsigned c : {unsigned{code::vccz}, unsigned{code::execz}, unsigned{code::scc}}) {
        SCOPED_TRACE(c);
        EXPECT_EQ(one({sop1(3, 0, c)}).src[0], reg(c));
        EXPECT_EQ(one({sop1(4, 0, c)}).src[0], reg(c, 2));
    }

    // The next instruction starts behind the literal.
    auto insts = decode(std::vector<std::uint32_t>{sop1(3, 0, 255), 0x42, sopp(1)});
    ASSERT_EQ(insts.size(), 2u);
    EXPECT_EQ(insts[1].offset, 8u);
}

TEST(GcnDecode, StopsAtTheFinalEndpgm) {
    // An early s_endpgm that a branch skips, then the final one, then data that isn't code.
    const std::vector<std::uint32_t> code = {
        sopp(4, 1),  // 0x00: s_cbranch_scc0 to 0x08
        sopp(1),     // 0x04: s_endpgm, before the branch target
        sopp(0, 1),  // 0x08: s_nop 1
        sopp(1),     // 0x0c: s_endpgm
        sopp(27),    // not an instruction
    };
    auto insts = decode(code);
    ASSERT_EQ(insts.size(), 4u);
    EXPECT_EQ(insts.back().offset, 0x0cu);

    // A branch exactly to an s_endpgm ends the program there.
    EXPECT_EQ(decode(std::vector<std::uint32_t>{sopp(2, 0), sopp(1), sopp(1)}).size(), 2u);

    auto e = error({sopp(4, 1), sopp(1), sopp(0)}, Until::program_end);
    EXPECT_EQ(e.offset(), 12u);
    EXPECT_NE(std::string(e.what()).find("no s_endpgm"), std::string::npos);
    std::vector<std::uint32_t> all(code.begin(), code.end() - 1);
    EXPECT_EQ(decode(all, Until::code_end).size(), 4u);
}

// The dwords that decide how a program decodes: all of it, or up to the failing instruction.
TEST(Decode, ReadableExtent) {
    const std::uint32_t endpgm = 0xbf810000u;
    EXPECT_EQ(readable_extent(std::vector<std::uint32_t>{endpgm, 0, 0}, Until::program_end), 1u);
    EXPECT_EQ(readable_extent(std::vector<std::uint32_t>{0xffffffffu, 0, 0, 0, 0}, Until::program_end), 2u);
    EXPECT_EQ(readable_extent(std::vector<std::uint32_t>{0xffffffffu}, Until::program_end), 1u);
    EXPECT_EQ(readable_extent(std::vector<std::uint32_t>{endpgm}, Until::subroutine_end), 1u);  // runs out of code
}

}  // namespace
