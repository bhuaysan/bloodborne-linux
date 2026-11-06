// Encoders for test words, written from the microcode format diagrams of the Sea Islands ISA
// manual.
#pragma once

#include <cstdint>
#include <vector>

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

// The vector memory formats. Flags are the bits of the 64-bit encoding: the low half is the first
// dword and the high half the second. Descriptors are given by their first SGPR (a multiple of 4).
inline std::vector<std::uint32_t> split(std::uint64_t encoding) {
    return {static_cast<std::uint32_t>(encoding), static_cast<std::uint32_t>(encoding >> 32)};
}

constexpr std::uint64_t second(unsigned field, unsigned low) {
    return std::uint64_t{field} << (32 + low);
}

namespace buf {  // MUBUF
inline constexpr std::uint64_t offen = 1u << 12, idxen = 1u << 13, glc = 1u << 14,
                               addr64 = 1u << 15, lds = 1u << 16;
inline constexpr std::uint64_t slc = second(1, 22), tfe = second(1, 23);
}  // namespace buf

inline std::vector<std::uint32_t> mubuf(unsigned op, unsigned vdata, unsigned vaddr,
                                        unsigned srsrc, unsigned soffset, unsigned offset = 0,
                                        std::uint64_t flags = 0) {
    return split(0x38ull << 26 | op << 18 | offset | second(vaddr, 0) | second(vdata, 8) |
                 second(srsrc / 4, 16) | second(soffset, 24) | flags);
}

// `src0` is the 9-bit operand code (256 + n for vn). `vdst` is a VGPR number, or the operand code
// of an SGPR destination.
constexpr std::uint32_t vop1(unsigned op, unsigned vdst, unsigned src0) {
    return 0x3fu << 25 | vdst << 17 | op << 9 | src0;
}

}  // namespace bb::test::gcn
