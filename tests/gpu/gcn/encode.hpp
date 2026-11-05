// Encoders for test words, written from the microcode format diagrams of the Sea Islands ISA
// manual.
#pragma once

#include <cstdint>

namespace bb::test::gcn {

constexpr std::uint32_t sopp(unsigned op, unsigned simm16 = 0) {
    return 0x17fu << 23 | op << 16 | (simm16 & 0xffff);
}

}  // namespace bb::test::gcn
