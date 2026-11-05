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

    EXPECT_EQ(error({0x00000000}).word(), 0u);  // VOP2, not decoded yet
    EXPECT_EQ(error({sopp(2, 0xfffe)}).word(), sopp(2, 0xfffe));  // before the start
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

}  // namespace
