// Encoders for test words, written from the microcode format diagrams of the Sea Islands ISA
// manual.
#pragma once

#include <cstdint>

namespace bb::test::gcn {

constexpr std::uint32_t sop2(unsigned op, unsigned sdst, unsigned ssrc0, unsigned ssrc1) {
    return 2u << 30 | op << 23 | sdst << 16 | ssrc1 << 8 | ssrc0;
}

constexpr std::uint32_t sopk(unsigned op, unsigned sdst, unsigned simm16) {
    return 0xbu << 28 | op << 23 | sdst << 16 | simm16;
}

constexpr std::uint32_t sop1(unsigned op, unsigned sdst, unsigned ssrc0) {
    return 0x17du << 23 | sdst << 16 | op << 8 | ssrc0;
}

constexpr std::uint32_t sopc(unsigned op, unsigned ssrc0, unsigned ssrc1) {
    return 0x17eu << 23 | op << 16 | ssrc1 << 8 | ssrc0;
}

constexpr std::uint32_t sopp(unsigned op, unsigned simm16 = 0) {
    return 0x17fu << 23 | op << 16 | (simm16 & 0xffff);
}

// `sbase` is the first SGPR of the base address (even).
constexpr std::uint32_t smrd(unsigned op, unsigned sdst, unsigned sbase, bool imm,
                             unsigned offset) {
    return 0x18u << 27 | op << 22 | sdst << 15 | (sbase / 2) << 9 | unsigned{imm} << 8 | offset;
}

}  // namespace bb::test::gcn
