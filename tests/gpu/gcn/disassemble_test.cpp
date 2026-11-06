// Disassembler tests: LLVM's syntax for words encoded by hand from the Sea Islands ISA manual.
// With llvm-mc installed, every line is also assembled back to its words.

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

struct Case {
    std::vector<std::uint32_t> words;  // one instruction
    std::string text;
};

const std::vector<Case> cases = {
    // SOP2: registers, special registers, inline constants, a literal read by both sources.
    {{sop2(0, 0, 1, 2)}, "s_add_u32 s0, s1, s2"},
    {{sop2(15, code::exec, code::vcc, 193)}, "s_and_b64 exec, vcc, -1"},
    {{sop2(31, 2, 4, 6)}, "s_lshl_b64 s[2:3], s[4:5], s6"},
    {{sop2(43, 0, 4, 8)}, "s_cbranch_g_fork s[4:5], s[8:9]"},
    {{sop2(43, 0, 193, 8)}, "s_cbranch_g_fork -1, s[8:9]"},  // inline constants, but no literal
    {{sop2(2, code::m0, 255, 255), 0x12345}, "s_add_i32 m0, 0x12345, 0x12345"},
    // Literals with an inline value keep their encoding through lit().
    {{sop2(2, 0, 255, 255), 0xfffffff0}, "s_add_i32 s0, lit(0xfffffff0), lit(0xfffffff0)"},
    {{sop2(11, 0, 255, 2), 0x40}, "s_cselect_b64 s[0:1], lit(0x40), s[2:3]"},
    {{sop2(11, 0, 255, 2), 0xfffffff0}, "s_cselect_b64 s[0:1], 0xfffffff0, s[2:3]"},
    {{sop2(31, 0, 2, 255), 0x3f800000}, "s_lshl_b64 s[0:1], s[2:3], lit(0x3f800000)"},
    // SOPK: hex immediates, hardware registers, a branch.
    {{sopk(0, 0, 0x1234)}, "s_movk_i32 s0, 0x1234"},
    {{sopk(10, 5, 0x8000)}, "s_cmpk_lg_u32 s5, 0x8000"},
    {{sopk(18, 0, 0xf801)}, "s_getreg_b32 s0, hwreg(HW_REG_MODE)"},
    {{sopk(18, 0, 0x0000)}, "s_getreg_b32 s0, hwreg(0, 0, 1)"},
    {{sopk(18, 0, 0xffff)}, "s_getreg_b32 s0, hwreg(63, 31, 32)"},
    {{sopk(19, 3, 4 | 4 << 6 | 7 << 11)}, "s_setreg_b32 hwreg(HW_REG_HW_ID, 4, 8), s3"},
    {{sopk(21, 0, 9 | 3 << 11), 3}, "s_setreg_imm32_b32 hwreg(9, 0, 4), 3"},
    {{sopk(21, 0, 1), 0xfffffff0}, "s_setreg_imm32_b32 hwreg(HW_REG_MODE, 0, 1), -16"},
    {{sopk(21, 0, 1), 0x41}, "s_setreg_imm32_b32 hwreg(HW_REG_MODE, 0, 1), 0x41"},
    {{sopk(21, 0, 1), 0x3f800000}, "s_setreg_imm32_b32 hwreg(HW_REG_MODE, 0, 1), 0x3f800000"},
    {{sopk(17, 4, 0xffff)}, "s_cbranch_i_fork s[4:5], -1"},
    // SOP1: register names of every kind, constants of both sizes.
    {{sop1(4, code::vcc, code::exec)}, "s_mov_b64 vcc, exec"},
    {{sop1(3, code::exec + 1, code::ttmp0 + 11)}, "s_mov_b32 exec_hi, ttmp11"},
    {{sop1(4, code::ttmp0 + 2, code::flat_scratch)}, "s_mov_b64 ttmp[2:3], flat_scratch"},
    {{sop1(3, 103, code::tma + 1)}, "s_mov_b32 s103, tma_hi"},
    {{sop1(3, code::tba, code::flat_scratch + 1)}, "s_mov_b32 tba_lo, flat_scratch_hi"},
    {{sop1(3, code::vcc + 1, code::m0)}, "s_mov_b32 vcc_hi, m0"},
    {{sop1(4, 0, code::tba)}, "s_mov_b64 s[0:1], tba"},
    {{sop1(3, 0, code::scc)}, "s_mov_b32 s0, src_scc"},
    {{sop1(3, 0, code::vccz)}, "s_mov_b32 s0, src_vccz"},
    {{sop1(3, 0, code::execz)}, "s_mov_b32 s0, src_execz"},
    {{sop1(4, 0, code::vccz)}, "s_mov_b64 s[0:1], src_vccz"},
    {{sop1(3, 0, 192)}, "s_mov_b32 s0, 64"},
    {{sop1(3, 0, 208)}, "s_mov_b32 s0, -16"},
    {{sop1(3, 0, 241)}, "s_mov_b32 s0, -0.5"},
    {{sop1(4, 0, 247)}, "s_mov_b64 s[0:1], -4.0"},
    {{sop1(4, 0, 208)}, "s_mov_b64 s[0:1], -16"},
    {{sop1(3, 0, 255), 0x3f000000}, "s_mov_b32 s0, lit(0x3f000000)"},
    {{sop1(3, 0, 255), 0x41}, "s_mov_b32 s0, 0x41"},
    {{sop1(4, 0, 255), 0x3f800000}, "s_mov_b64 s[0:1], 0x3f800000"},
    {{sop1(36, 2, code::vcc)}, "s_and_saveexec_b64 s[2:3], vcc"},
    {{sop1(31, 0, 0)}, "s_getpc_b64 s[0:1]"},
    {{sop1(32, 0, 4)}, "s_setpc_b64 s[4:5]"},
    {{sop1(32, 0, code::vcc)}, "s_setpc_b64 vcc"},
    {{sop1(50, 0, code::scc)}, "s_cbranch_join src_scc"},
    {{sop1(16, 0, 2)}, "s_bcnt1_i32_b64 s0, s[2:3]"},
    // SOPC.
    {{sopc(6, 0, 242)}, "s_cmp_eq_u32 s0, 1.0"},
    {{sopc(15, 4, 9)}, "s_bitcmp1_b64 s[4:5], s9"},
    {{sopc(16, 0, 255), 0x7}, "s_setvskip s0, lit(0x7)"},
    // SOPP: counts, branches, s_waitcnt and s_sendmsg.
    {{sopp(1)}, "s_endpgm"},
    {{sopp(0, 7)}, "s_nop 7"},
    {{sopp(0, 64)}, "s_nop 64"},  // counts above 64 print in hex
    {{sopp(13, 65)}, "s_sethalt 0x41"},
    {{sopp(18, 0xffff)}, "s_trap 0xffff"},
    {{sopp(4, 0x0002)}, "s_cbranch_scc0 2"},
    {{sopp(2, 0xffff)}, "s_branch -1"},
    {{sopp(12, 0x007f)}, "s_waitcnt lgkmcnt(0)"},
    {{sopp(12, 0x0f70)}, "s_waitcnt vmcnt(0)"},
    {{sopp(12, 0x0f00)}, "s_waitcnt vmcnt(0) expcnt(0)"},
    {{sopp(12, 0x0000)}, "s_waitcnt vmcnt(0) expcnt(0) lgkmcnt(0)"},
    {{sopp(12, 0x0f7f)}, "s_waitcnt vmcnt(15) expcnt(7) lgkmcnt(15)"},
    {{sopp(12, 0x1f7f)}, "s_waitcnt 0x1f7f"},  // bit 12 is outside lgkmcnt
    {{sopp(12, 0x0fff)}, "s_waitcnt 0xfff"},   // so is bit 7
    {{sopp(16, 0x0001)}, "s_sendmsg sendmsg(MSG_INTERRUPT)"},
    {{sopp(16, 0x0003)}, "s_sendmsg sendmsg(MSG_GS_DONE, GS_OP_NOP)"},
    {{sopp(16, 0x0122)}, "s_sendmsg sendmsg(MSG_GS, GS_OP_EMIT, 1)"},
    {{sopp(16, 0x0312)}, "s_sendmsg sendmsg(MSG_GS, GS_OP_CUT, 3)"},
    {{sopp(16, 0x0033)}, "s_sendmsg sendmsg(MSG_GS_DONE, GS_OP_EMIT_CUT, 0)"},
    {{sopp(17, 0x002f)}, "s_sendmsghalt sendmsg(MSG_SYSMSG, SYSMSG_OP_REG_RD)"},
    {{sopp(16, 0x004f)}, "s_sendmsg sendmsg(MSG_SYSMSG, SYSMSG_OP_TTRACE_PC)"},
    {{sopp(16, 0x0002)}, "s_sendmsg 2"},       // MSG_GS needs an operation
    {{sopp(16, 0x0103)}, "s_sendmsg 259"},     // GS_OP_NOP has no stream
    {{sopp(16, 0x005f)}, "s_sendmsg 95"},      // no system message 5
    {{sopp(16, 0xffff)}, "s_sendmsg 65535"},
    // SMRD.
    {{smrd(2, 0, 4, true, 0x10)}, "s_load_dwordx4 s[0:3], s[4:5], 0x10"},
    {{smrd(12, 16, 8, true, 0)}, "s_buffer_load_dwordx16 s[16:31], s[8:11], 0x0"},
    {{smrd(8, code::ttmp0 + 4, code::ttmp0 + 8, true, 4)},
     "s_buffer_load_dword ttmp4, ttmp[8:11], 0x4"},
    {{smrd(2, 100, code::tba, false, code::m0)}, "s_load_dwordx4 s[100:103], tba, m0"},
    {{smrd(0, 0, 4, false, 255), 0x12345}, "s_load_dword s0, s[4:5], 0x12345"},
    {{smrd(30, 2, 0, false, 0)}, "s_memtime s[2:3]"},
    {{smrd(31, 0, 0, false, 0)}, "s_dcache_inv"},
};

std::string text(const std::vector<std::uint32_t>& words) {
    auto insts = decode(words, Until::code_end);
    EXPECT_EQ(insts.size(), 1u);
    return disassemble(insts.at(0));
}

TEST(GcnDisassemble, Text) {
    for (const Case& c : cases) {
        SCOPED_TRACE(c.text);
        EXPECT_EQ(text(c.words), c.text);
    }
}

TEST(GcnDisassemble, AssemblesToTheSameWords) {
    if (!have_llvm_mc()) {
        GTEST_SKIP() << "llvm-mc not found";
    }
    std::vector<std::string> lines;
    for (const Case& c : cases) {
        lines.push_back(c.text);
    }
    const auto assembled = assemble(lines);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        EXPECT_EQ(assembled[i].words, cases[i].words) << cases[i].text;
    }
}

// Encodings that LLVM's syntax can't express. The text is still LLVM's syntax.
TEST(GcnDisassemble, EncodingsLlvmCannotExpress) {
    // LLVM rejects m0 and exec (or either half) as an SMRD destination, which the field allows.
    EXPECT_EQ(text({smrd(0, code::m0, 4, false, 6)}), "s_load_dword m0, s[4:5], s6");
    EXPECT_EQ(text({smrd(0, code::exec + 1, 4, true, 1)}), "s_load_dword exec_hi, s[4:5], 0x1");
    EXPECT_EQ(text({smrd(1, code::exec, 4, true, 1)}), "s_load_dwordx2 exec, s[4:5], 0x1");
    // A literal offset of 255 or less: LLVM encodes the immediate form, with or without lit().
    EXPECT_EQ(text({smrd(0, 0, 4, false, 255), 0x10}), "s_load_dword s0, s[4:5], 0x10");
    // Opcodes with no gfx7 syntax in LLVM.
    EXPECT_EQ(text({sop1(51, 0, 1)}), "s_mov_regrd_b32 s0, s1");
    EXPECT_EQ(text({sopk(20, 0, 0xf801)}), "s_getreg_regrd_b32 s0, hwreg(HW_REG_MODE)");
}

// Sources that LLVM takes only as a register, or for s_cbranch_g_fork not as a literal. The operand
// field allows any source, so the decoder accepts these and llvm-mc rejects the text.
TEST(GcnDisassemble, SourcesLlvmTakesOnlyAsRegisters) {
    const std::vector<Case> rejected = {
        {{sop1(32, 0, 128)}, "s_setpc_b64 0"},
        {{sop1(34, 0, 255), 0}, "s_rfe_b64 lit(0x0)"},
        {{sop1(50, 0, 255), 0x12345}, "s_cbranch_join 0x12345"},
        {{sop1(46, 0, 242)}, "s_movrels_b32 s0, 1.0"},
        {{sop1(47, 0, 193)}, "s_movrels_b64 s[0:1], -1"},
        {{sop2(43, 0, 255, 8), 0}, "s_cbranch_g_fork lit(0x0), s[8:9]"},
        {{sop2(43, 0, 4, 255), 0x12345}, "s_cbranch_g_fork s[4:5], 0x12345"},
    };
    for (const Case& c : rejected) {
        SCOPED_TRACE(c.text);
        EXPECT_EQ(text(c.words), c.text);
        if (have_llvm_mc()) {
            EXPECT_THROW(assemble({c.text}), std::runtime_error);
        }
    }
}

}  // namespace
