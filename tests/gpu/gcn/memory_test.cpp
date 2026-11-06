// MUBUF: decoding and LLVM's syntax for words encoded by hand from the Sea Islands ISA manual,
// and a round trip through llvm-mc for every opcode of the table.

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

Instruction one(const std::vector<std::uint32_t>& words) {
    auto insts = decode(words, Until::code_end);
    EXPECT_EQ(insts.size(), 1u);
    return insts.at(0);
}

std::string text(const std::vector<std::uint32_t>& words) {
    return disassemble(one(words));
}

std::string error(const std::vector<std::uint32_t>& words) {
    try {
        decode(words, Until::code_end);
    } catch (const GcnError& e) {
        return e.what();
    }
    return "no error";
}

Operand v(unsigned first, std::uint8_t dwords = 1) {
    return {OperandKind::vector, dwords, static_cast<std::uint16_t>(first), 0};
}

TEST(GcnMemory, Mubuf) {
    // A load writes vdata, with a status dword after the data when tfe is set.
    Instruction inst = one(mubuf(12, 1, 2, 4, 3, 0x123, buf::offen | buf::glc | buf::tfe));
    EXPECT_EQ(inst.format, Format::mubuf);
    EXPECT_EQ(inst.size, 2);
    EXPECT_EQ(inst.num_dst, 1);
    EXPECT_EQ(inst.dst[0], v(1, 2));
    ASSERT_EQ(inst.num_src, 3);
    EXPECT_EQ(inst.src[0], v(2));
    EXPECT_EQ(inst.src[1], (Operand{OperandKind::scalar, 4, 4, 0}));
    EXPECT_EQ(inst.src[2], (Operand{OperandKind::scalar, 1, 3, 0}));
    EXPECT_EQ(inst.mem_offset, 0x123);
    EXPECT_EQ(inst.flags, flag::offen | flag::glc | flag::tfe);

    // Stores and atomics read vdata. A load into LDS has none.
    inst = one(mubuf(81, 4, 0, 0, 241));  // buffer_atomic_cmpswap_x2
    EXPECT_EQ(inst.num_dst, 0);
    ASSERT_EQ(inst.num_src, 4);
    EXPECT_EQ(inst.src[0], v(4, 4));
    EXPECT_EQ(inst.src[1], (Operand{OperandKind::off, 0, 0, 0}));
    EXPECT_EQ(inst.src[3], (Operand{OperandKind::constant, 1, 241, 0xbf000000}));  // -0.5
    inst = one(mubuf(12, 0, 0, 4, 1, 0, buf::lds));
    EXPECT_EQ(inst.num_dst, 0);
    EXPECT_EQ(inst.num_src, 3);
}

TEST(GcnMemory, Errors) {
    EXPECT_NE(error(mubuf(52, 1, 0, 4, 1)).find("unknown MUBUF opcode 52 at offset 0x0"),
              std::string::npos);
    EXPECT_NE(error({mubuf(12, 1, 0, 4, 1)[0]}).find("second dword past the end"),
              std::string::npos);
    EXPECT_NE(error(mubuf(28, 1, 0, 4, 1, 0, buf::lds)).find("lds bit on a store"),
              std::string::npos);
    EXPECT_NE(error(mubuf(14, 253, 0, 4, 1)).find("VGPR range v253 with 4 dwords past v255"),
              std::string::npos);
    EXPECT_NE(error(mubuf(12, 1, 0, 4, 255)).find("invalid soffset operand 255"),  // no literal
              std::string::npos);
    EXPECT_NE(error(mubuf(12, 1, 0, 104, 1)).find("invalid 4-dword descriptor operand 104"),
              std::string::npos);
}

struct Case {
    std::vector<std::uint32_t> words;
    std::string text;
};

// Address modes, fields, the soffset forms, a load into LDS.
const std::vector<Case> cases = {
    {mubuf(12, 1, 0, 4, 1), "buffer_load_dword v1, off, s[4:7], s1"},
    {mubuf(12, 1, 2, 4, 1, 4095, buf::offen | buf::glc | buf::slc | buf::tfe),
     "buffer_load_dword v[1:2], v2, s[4:7], s1 offen offset:4095 glc slc tfe"},
    {mubuf(3, 1, 2, 8, 128, 0, buf::idxen | buf::offen),
     "buffer_load_format_xyzw v[1:4], v[2:3], s[8:11], 0 idxen offen"},
    {mubuf(12, 255, 254, 4, 128, 0, buf::addr64),
     "buffer_load_dword v255, v[254:255], s[4:7], 0 addr64"},
    {mubuf(28, 1, 0, 116, code::m0), "buffer_store_dword v1, off, ttmp[4:7], m0"},
    {mubuf(31, 1, 0, 4, 242, 8), "buffer_store_dwordx3 v[1:3], off, s[4:7], 1.0 offset:8"},
    {mubuf(50, 1, 2, 4, 193, 0, buf::offen | buf::glc),
     "buffer_atomic_add v1, v2, s[4:7], -1 offen glc"},
    {mubuf(62, 2, 0, 4, code::vcc + 1), "buffer_atomic_fcmpswap v[2:3], off, s[4:7], vcc_hi"},
    {mubuf(12, 0, 2, 4, 1, 8, buf::offen | buf::lds),
     "buffer_load_dword v2, s[4:7], s1 offen offset:8 lds"},
    {mubuf(112, 0, 0, 0, 0), "buffer_wbinvl1_vol"},
};

TEST(GcnMemory, Text) {
    for (const Case& c : cases) {
        SCOPED_TRACE(c.text);
        EXPECT_EQ(text(c.words), c.text);
    }
}

// The words llvm-mc assembles from the text of each case.
void expect_assembles(const std::vector<Case>& cases) {
    std::vector<std::string> lines;
    for (const Case& c : cases) {
        lines.push_back(c.text);
    }
    const auto assembled = assemble(lines);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        EXPECT_EQ(assembled[i].words, cases[i].words) << cases[i].text;
    }
}

TEST(GcnMemory, AssemblesToTheSameWords) {
    if (!have_llvm_mc()) {
        GTEST_SKIP() << "llvm-mc not found";
    }
    expect_assembles(cases);
}

// Field combinations that LLVM's syntax can't express. The decoder accepts them, and llvm-mc
// rejects the text or encodes it differently.
TEST(GcnMemory, EncodingsLlvmCannotExpress) {
    const std::vector<Case> differ = {
        {mubuf(28, 1, 0, 4, 1, 0, buf::tfe), "buffer_store_dword v1, off, s[4:7], s1 tfe"},
        {mubuf(50, 1, 0, 4, 1, 0, buf::tfe), "buffer_atomic_add v1, off, s[4:7], s1 tfe"},
        {mubuf(12, 1, 2, 4, 1, 0, buf::addr64 | buf::offen),
         "buffer_load_dword v1, v[2:3], s[4:7], s1 offen addr64"},
        {mubuf(12, 0, 0, 4, 1, 0, buf::lds | buf::tfe),
         "buffer_load_dword off, s[4:7], s1 lds tfe"},
        {mubuf(13, 0, 0, 4, 1, 0, buf::lds), "buffer_load_dwordx2 off, s[4:7], s1 lds"},
        // Fields that an instruction doesn't use don't appear in the text.
        {mubuf(113, 1, 2, 4, 1), "buffer_wbinvl1"},
    };
    for (const Case& c : differ) {
        SCOPED_TRACE(c.text);
        EXPECT_EQ(text(c.words), c.text);
        if (have_llvm_mc()) {
            try {
                EXPECT_NE(assemble({c.text}).at(0).words, c.words);
            } catch (const std::runtime_error&) {
                // rejected
            }
        }
    }
}

// Every opcode of the table, with typical operands, through llvm-mc and back.
TEST(GcnMemory, RoundTripMubuf) {
    std::vector<Case> all;
    for (unsigned op = 0; op < 128; ++op) {
        // The cache invalidations have no operands, so their fields are 0.
        const auto w = op >= 112 ? mubuf(op, 0, 0, 0, 0) : mubuf(op, 1, 6, 4, 2, 4, buf::offen);
        try {
            all.push_back({w, text(w)});
        } catch (const GcnError&) {
        }
    }
    EXPECT_EQ(all.size(), 56u);
    if (!have_llvm_mc()) {
        GTEST_SKIP() << "llvm-mc not found";
    }
    expect_assembles(all);
}

}  // namespace
