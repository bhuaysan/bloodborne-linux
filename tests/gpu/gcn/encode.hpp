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

// MTBUF: the layout of MUBUF with a 3-bit opcode and the data and numeric formats in its place.
inline std::vector<std::uint32_t> mtbuf(unsigned op, unsigned vdata, unsigned vaddr,
                                        unsigned srsrc, unsigned soffset, unsigned offset,
                                        unsigned dfmt, unsigned nfmt, std::uint64_t flags = 0) {
    return split(0x3aull << 26 | op << 16 | dfmt << 19 | nfmt << 23 | offset | second(vaddr, 0) |
                 second(vdata, 8) | second(srsrc / 4, 16) | second(soffset, 24) | flags);
}

namespace img {  // MIMG: the flags are bits of the first dword
inline constexpr std::uint64_t unorm = 1u << 12, glc = 1u << 13, da = 1u << 14, r128 = 1u << 15,
                               tfe = 1u << 16, lwe = 1u << 17, slc = 1u << 25;
}  // namespace img

// MIMG: vdata and vaddr are VGPR numbers, srsrc and ssamp their first SGPR (a multiple of 4).
inline std::vector<std::uint32_t> mimg(unsigned op, unsigned vdata, unsigned vaddr, unsigned srsrc,
                                       unsigned ssamp, unsigned dmask, std::uint64_t flags = 0) {
    return split(0x3cull << 26 | op << 18 | dmask << 8 | flags | second(vaddr, 0) |
                 second(vdata, 8) | second(srsrc / 4, 16) | second(ssamp / 4, 21));
}

namespace xp {  // EXP
inline constexpr std::uint64_t compr = 1u << 10, done = 1u << 11, vm = 1u << 12;
}  // namespace xp

inline std::vector<std::uint32_t> xport(unsigned en, unsigned target, unsigned vsrc0,
                                      unsigned vsrc1, unsigned vsrc2, unsigned vsrc3,
                                      std::uint64_t flags = 0) {
    return split(0x3eull << 26 | en | target << 4 | flags | second(vsrc0, 0) | second(vsrc1, 8) |
                 second(vsrc2, 16) | second(vsrc3, 24));
}

// `src0` is the 9-bit operand code (256 + n for vn). `vdst` is a VGPR number, or the operand code
// of an SGPR destination.
constexpr std::uint32_t vop1(unsigned op, unsigned vdst, unsigned src0) {
    return 0x3fu << 25 | vdst << 17 | op << 9 | src0;
}

// VOP2 and VOPC. `vdst` is a VGPR number, or the operand code of an SGPR destination for
// v_readlane_b32. `src0` is the 9-bit operand code, `vsrc1` a VGPR number (or the 8-bit operand
// code of the lane select of v_readlane_b32 and v_writelane_b32).
constexpr std::uint32_t vop2(unsigned op, unsigned vdst, unsigned src0, unsigned vsrc1) {
    return op << 25 | vdst << 17 | vsrc1 << 9 | src0;
}

constexpr std::uint32_t vopc(unsigned op, unsigned src0, unsigned vsrc1) {
    return 0x3eu << 25 | op << 17 | vsrc1 << 9 | src0;
}

// VOP3. `low` is ORed into the first dword (sdst << 8 for VOP3b) and `high` into the second. The
// sources are 9-bit operand codes.
inline std::vector<std::uint32_t> vop3(unsigned op, unsigned vdst, unsigned src0, unsigned src1 = 0,
                                       unsigned src2 = 0, std::uint32_t low = 0,
                                       std::uint32_t high = 0) {
    return {0x34u << 26 | op << 17 | low | vdst, src2 << 18 | src1 << 9 | src0 | high};
}

}  // namespace bb::test::gcn
