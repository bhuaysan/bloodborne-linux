// VOP1: decoding of words encoded by hand from the Sea Islands ISA manual.

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "gpu/gcn/encode.hpp"
#include "gpu/gcn/gcn.hpp"

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
        {0x00000000},       // VOP2, not decoded yet
        {0x7c000000},       // VOPC, not decoded yet
    };
    for (const auto& words : bad) {
        SCOPED_TRACE(words[0]);
        EXPECT_THROW(decode(words, Until::code_end), GcnError);
    }
}

}  // namespace
