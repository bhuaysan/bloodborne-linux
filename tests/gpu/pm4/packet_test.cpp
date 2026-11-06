#include <array>
#include <cstdint>
#include <optional>

#include <gtest/gtest.h>

#include "gpu/pm4/packet.hpp"

namespace {

using bb::pm4::Packet;
using bb::pm4::ShaderType;
using bb::pm4::type3_header;

TEST(Pm4Packet, Type3HeaderFields) {
    const std::uint32_t header = type3_header(IT_SET_CONTEXT_REG, 2, ShaderType::graphics, true);
    EXPECT_EQ(header, 0xC0016901u);
    EXPECT_EQ(bb::pm4::header_type(header), 3u);
    EXPECT_EQ(bb::pm4::body_dwords(header), 2u);
    const Packet packet{header, {}};
    EXPECT_EQ(packet.opcode(), IT_SET_CONTEXT_REG);
    EXPECT_TRUE(packet.predicate());
    EXPECT_EQ(packet.shader_type(), ShaderType::graphics);

    const Packet compute{type3_header(IT_SET_SH_REG, 3, ShaderType::compute), {}};
    EXPECT_EQ(compute.header, 0xC0027602u);
    EXPECT_EQ(compute.shader_type(), ShaderType::compute);
    EXPECT_FALSE(compute.predicate());
}

TEST(Pm4Packet, CountOf3FFFIsAnEmptyBody) {
    EXPECT_EQ(type3_header(IT_NOP, 0), 0xFFFF1000u);
    EXPECT_EQ(bb::pm4::body_dwords(0xFFFF1000u), 0u);
    EXPECT_EQ(bb::pm4::body_dwords(0xC0001000u), 1u);  // the CB/DB extent hint NOP
    EXPECT_EQ(bb::pm4::body_dwords(0x00010100u), 2u);  // type 0: two registers
    EXPECT_EQ(bb::pm4::header_type(bb::pm4::type2_filler), 2u);
}

TEST(Pm4Packet, NopTags) {
    const std::array<std::uint32_t, 3> flip = {0x68750777, 0, 0};
    const std::array<std::uint32_t, 2> unchanged = {0xC01E0000, 7};
    const std::uint32_t nop = type3_header(IT_NOP, 3);
    EXPECT_EQ(bb::pm4::nop_tag({nop, flip}), 0x68750777u);
    EXPECT_EQ(bb::pm4::nop_tag({nop, unchanged}), std::nullopt);
    EXPECT_EQ(bb::pm4::nop_tag({type3_header(IT_NOP, 0), {}}), std::nullopt);
    EXPECT_EQ(bb::pm4::nop_tag({type3_header(IT_SET_SH_REG, 3), flip}), std::nullopt);
}

}  // namespace
