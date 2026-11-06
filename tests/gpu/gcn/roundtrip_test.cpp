// Round trips through LLVM's assembler (docs/design/graphics.md, "Offline test strategy"): our
// disassembly of every scalar opcode in the tables, written from the Sea Islands ISA manual, of
// every valid operand code and of the immediate forms is assembled by llvm-mc. It must give the
// original words, and LLVM must print the same text back.

#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "gpu/gcn/encode.hpp"
#include "gpu/gcn/gcn.hpp"
#include "gpu/gcn/llvm_mc.hpp"
#include "gpu/gcn/opcodes.hpp"

namespace {

using namespace bb::gcn;
using namespace bb::test::gcn;
using bb::gcn::detail::Imm;
using bb::gcn::detail::OpInfo;

using Words = std::vector<std::uint32_t>;

// Opcodes of the manual that LLVM has no gfx7 syntax for, tested by hand below.
const std::set<std::string_view> not_in_llvm = {"s_mov_regrd_b32", "s_mov_fed_b32",
                                                "s_getreg_regrd_b32"};

constexpr unsigned hw_reg_mode = 1 | 3 << 6 | 3 << 11;  // hwreg(HW_REG_MODE, 3, 4)

// One instruction with opcode `op`: the destination at s0, the first source (or the SMRD base)
// at s8 and the second source at s16.
Words example(Format format, unsigned op, const OpInfo& info) {
    const unsigned src0 = info.src0 ? 8 : 0;
    const unsigned src1 = info.src1 ? 16 : 0;
    switch (format) {
    case Format::sop2:
        return {sop2(op, 0, src0, src1)};
    case Format::sop1:
        return {sop1(op, 0, src0)};
    case Format::sopc:
        return {sopc(op, src0, src1)};
    case Format::smrd:
        return {smrd(op, 0, src0, info.src1 != 0, info.src1 ? 0x10 : 0)};
    default:
        break;
    }
    const unsigned sdst = info.dst ? 0 : src0;  // SOPK's sdst field holds either
    switch (info.imm) {
    case Imm::none:
        return {sopp(op)};
    case Imm::simm:
        return {sopk(op, sdst, 0x1234)};
    case Imm::branch:
        return {format == Format::sopk ? sopk(op, sdst, 3) : sopp(op, 3)};
    case Imm::hwreg:
    case Imm::setreg:
        return {sopk(op, sdst, hw_reg_mode)};
    case Imm::setreg_imm32:
        return {sopk(op, 0, hw_reg_mode), 0x1234};
    case Imm::count:
        return {sopp(op, 2)};
    case Imm::waitcnt:
        return {sopp(op, 0x0070)};
    case Imm::sendmsg:
        return {sopp(op, 0x0013)};
    }
    return {};
}

// A raw s_waitcnt or s_sendmsg value. LLVM prints it in its field syntax, which drops the bits
// outside the fields for s_waitcnt, so only the words are compared.
bool raw_immediate(std::string_view line) {
    const bool counters = line.starts_with("s_waitcnt ") || line.starts_with("s_sendmsg");
    const auto space = line.find(' ');
    return counters && space + 1 < line.size() && line[space + 1] >= '0' && line[space + 1] <= '9';
}

void append(Words& code, const Words& words) {
    code.insert(code.end(), words.begin(), words.end());
}

// Decodes `code`, assembles the disassembly with llvm-mc, and compares the words and the text.
void round_trip(const Words& code) {
    const auto insts = decode(code, Until::code_end);
    std::vector<std::string> lines;
    for (const auto& inst : insts) {
        lines.push_back(disassemble(inst));
    }
    const auto assembled = assemble(lines);
    for (std::size_t i = 0; i < insts.size(); ++i) {
        SCOPED_TRACE(lines[i]);
        const auto first = code.begin() + insts[i].offset / 4;
        EXPECT_EQ(assembled[i].words, Words(first, first + insts[i].size));
        if (!raw_immediate(lines[i])) {
            EXPECT_EQ(assembled[i].text, lines[i]);
        }
    }
}

TEST(GcnRoundTrip, EveryScalarOpcode) {
    if (!have_llvm_mc()) {
        GTEST_SKIP() << "llvm-mc not found";
    }
    Words code;
    std::vector<unsigned> counts;
    for (auto format : {Format::sop2, Format::sopk, Format::sop1, Format::sopc, Format::sopp,
                        Format::smrd}) {
        const auto& table = detail::opcodes(format);
        counts.push_back(0);
        for (unsigned op = 0; op < table.size(); ++op) {
            if (!table[op].name.empty() && !not_in_llvm.contains(table[op].name)) {
                append(code, example(format, op, table[op]));
                ++counts.back();
            }
        }
    }
    // SOP2, SOPK, SOP1, SOPC, SOPP and SMRD, without the opcodes LLVM has no syntax for.
    EXPECT_EQ(counts, (std::vector<unsigned>{43, 20, 48, 17, 26, 13}));
    round_trip(code);
}

// The words of s_mov_b32 (op 3) or s_mov_b64 (op 4) with each operand code that decodes as its
// source or destination, and in `count` how many codes those are.
Words valid_operands(unsigned op, bool source, unsigned& count) {
    Words code;
    count = 0;
    for (unsigned c = 0; c < (source ? 256u : 128u); ++c) {  // the destination field has 7 bits
        // The literal is an s_endpgm, so the words decode whether the instruction reads it or not.
        Words words = {source ? sop1(op, 0, c) : sop1(op, c, 0), sopp(1)};
        try {
            words.resize(decode(words, Until::code_end).at(0).size);
        } catch (const GcnError&) {
            continue;  // the decoder tests cover the invalid codes
        }
        append(code, words);
        ++count;
    }
    return code;
}

TEST(GcnRoundTrip, EveryOperandCode) {
    if (!have_llvm_mc()) {
        GTEST_SKIP() << "llvm-mc not found";
    }
    // 32-bit registers: s0-s103, the halves of flat_scratch, vcc, tba, tma and exec, ttmp0-11
    // and m0. 64-bit: the aligned pairs of those. Sources add the 89 inline constants, the 3
    // condition bits and the literal.
    struct Form {
        unsigned op;
        bool source;
        unsigned expected;
    };
    for (Form form : {Form{3, true, 127 + 93}, Form{4, true, 63 + 93}, Form{3, false, 127},
                      Form{4, false, 63}}) {
        SCOPED_TRACE(testing::Message() << "opcode " << form.op << ", source " << form.source);
        unsigned count = 0;
        const Words code = valid_operands(form.op, form.source, count);
        EXPECT_EQ(count, form.expected);
        round_trip(code);
    }
}

TEST(GcnRoundTrip, SmrdOffsetsAndBases) {
    if (!have_llvm_mc()) {
        GTEST_SKIP() << "llvm-mc not found";
    }
    round_trip({
        smrd(0, 0, 4, true, 0),  // immediate offsets
        smrd(0, 0, 4, true, 0xff),
        smrd(1, 2, 4, false, 9),  // SGPR offsets
        smrd(0, 0, 4, false, code::m0),
        smrd(2, 4, 4, false, code::literal), 0x100,  // literal offsets (CI)
        smrd(9, 0, 8, false, code::literal), 0x12345,
        smrd(1, code::vcc, code::ttmp0 + 2, true, 7),  // special registers as base and destination
        smrd(0, code::ttmp0, code::tba, true, 1),
        smrd(1, code::tma, code::vcc, true, 1),
        smrd(10, 8, code::ttmp0 + 4, false, 3),
    });
}

TEST(GcnRoundTrip, SoppAndSopkImmediates) {
    if (!have_llvm_mc()) {
        GTEST_SKIP() << "llvm-mc not found";
    }
    Words code;
    for (unsigned v : {0x0000u, 0x007fu, 0x0f00u, 0x0f70u, 0x0f7fu, 0x0fffu, 0x1f7fu, 0xffffu}) {
        append(code, {sopp(12, v)});  // s_waitcnt
    }
    for (unsigned v : {0x0001u, 0x0002u, 0x0003u, 0x0013u, 0x0122u, 0x0332u, 0x002fu, 0x004fu,
                       0x005fu, 0x0103u, 0x1234u, 0xffffu}) {
        append(code, {sopp(16, v), sopp(17, v)});  // s_sendmsg, s_sendmsghalt
    }
    for (unsigned id = 0; id < 10; ++id) {  // hwreg() with and without a name
        append(code, {sopk(18, 0, id | 31 << 11), sopk(19, 1, id | 5 << 6 | 7 << 11)});
    }
    append(code, {sopk(18, 0, 0x0000), sopk(18, 0, 0xffff), sopk(21, 0, 0xffff), 0xffffffff});
    for (unsigned v : {0x0000u, 0x7fffu, 0x8000u, 0xffffu}) {
        append(code, {sopk(0, 0, v), sopk(3, 1, v), sopp(0, v), sopp(18, v)});
    }
    // Branches back and forward. The code before them keeps the targets in range.
    append(code, {sopp(2, 0xfff0), sopp(5, 0x7fff), sopp(26, 0xffff), sopk(17, 2, 0xffe0)});
    round_trip(code);
}

// The opcodes without LLVM syntax, checked by hand against the manual's SOP1 and SOPK lists. They
// take the operands of their families (s_mov_b32, s_getreg_b32).
TEST(GcnRoundTrip, OpcodesWithoutLlvmSyntax) {
    struct Case {
        std::uint32_t word;
        std::string text;
    };
    const std::vector<Case> cases = {
        {sop1(51, 2, 9), "s_mov_regrd_b32 s2, s9"},
        {sop1(53, code::m0, 193), "s_mov_fed_b32 m0, -1"},
        {sopk(20, 4, 0xf802), "s_getreg_regrd_b32 s4, hwreg(HW_REG_STATUS)"},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.text);
        const auto insts = decode(Words{c.word}, Until::code_end);
        ASSERT_EQ(insts.size(), 1u);
        EXPECT_TRUE(not_in_llvm.contains(insts[0].name));
        EXPECT_EQ(insts[0].num_dst, 1);
        EXPECT_EQ(insts[0].num_src, 1);
        EXPECT_EQ(disassemble(insts[0]), c.text);
        if (have_llvm_mc()) {
            EXPECT_THROW(assemble({c.text}), std::runtime_error);  // LLVM still has no syntax
        }
    }
}

}  // namespace
