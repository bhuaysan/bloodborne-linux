#include <array>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "gpu/pm4/registers.hpp"

namespace {

using bb::pm4::RegisterMirror;
using bb::pm4::RegSpace;
using bb::pm4::Stage;

TEST(RegisterMirror, WritesEachSpaceAtItsBase) {
    RegisterMirror regs;
    const std::array<std::uint32_t, 2> values = {0x11, 0x22};
    EXPECT_EQ(regs.write(RegSpace::config, 0x10, values), 2u);
    EXPECT_EQ(regs.write(RegSpace::sh, 0x10, values), 2u);
    EXPECT_EQ(regs.write(RegSpace::context, 0x10, values), 2u);
    EXPECT_EQ(regs.write(RegSpace::uconfig, 0x3FFF, values), 1u);
    EXPECT_EQ(regs.get(0x2010), 0x11u);
    EXPECT_EQ(regs.get(0x2011), 0x22u);
    EXPECT_EQ(regs.get(0x2C11), 0x22u);
    EXPECT_EQ(regs.get(0xA010), 0x11u);
    EXPECT_EQ(regs.get(0xFFFF), 0x11u);
    EXPECT_EQ(regs.space(RegSpace::context)[0x11], 0x22u);
    EXPECT_EQ(regs.space(RegSpace::uconfig).size(), 0x4000u);
    // Outside the spaces, and the value past the uconfig space was dropped.
    EXPECT_EQ(regs.get(0x10000), 0u);
    EXPECT_EQ(regs.get(0x1FFF), 0u);
    EXPECT_EQ(regs.write(RegSpace::sh, 0x400, values), 0u);
    EXPECT_EQ(regs.get(0x3000), 0u);
}

TEST(RegisterMirror, ReadsStageRegisters) {
    RegisterMirror regs;
    // SPI_SHADER_PGM_LO_VS (0x2C48) to SPI_SHADER_USER_DATA_VS_1 (0x2C4D).
    const std::array<std::uint32_t, 6> vs = {0x00123456, 0x1AB, 0xC0FFEE, 0xBEEF, 7, 8};
    regs.write(RegSpace::sh, 0x48, vs);
    EXPECT_EQ(regs.program_address(Stage::vs), 0xAB0012345600u);
    EXPECT_EQ(regs.rsrc1(Stage::vs), 0xC0FFEEu);
    EXPECT_EQ(regs.rsrc2(Stage::vs), 0xBEEFu);
    EXPECT_EQ(regs.user_data(Stage::vs)[0], 7u);
    EXPECT_EQ(regs.user_data(Stage::vs)[1], 8u);
    EXPECT_EQ(regs.program_address(Stage::ps), 0u);

    // SPI_SHADER_USER_DATA_PS_15 (0x2C1B), LS_0 (0x2D4C).
    const std::array<std::uint32_t, 1> one = {0x55};
    regs.write(RegSpace::sh, 0x1B, one);
    regs.write(RegSpace::sh, 0x14C, one);
    EXPECT_EQ(regs.user_data(Stage::ps)[15], 0x55u);
    EXPECT_EQ(regs.user_data(Stage::ls)[0], 0x55u);

    // COMPUTE_NUM_THREAD_X..Z (0x2E07), COMPUTE_PGM_LO/HI (0x2E0C), USER_DATA_0 (0x2E40).
    const std::array<std::uint32_t, 3> threads = {0x40040, 2, 1};
    regs.write(RegSpace::sh, 0x207, threads);
    const std::array<std::uint32_t, 2> pgm = {0x1000, 0x2};
    regs.write(RegSpace::sh, 0x20C, pgm);
    regs.write(RegSpace::sh, 0x240, one);
    EXPECT_EQ(regs.num_threads(0), 0x40u);
    EXPECT_EQ(regs.num_threads(1), 2u);
    EXPECT_EQ(regs.num_threads(2), 1u);
    EXPECT_EQ(regs.num_threads(3), 0u);
    EXPECT_EQ(regs.program_address(Stage::cs), 0x20000100000u);
    EXPECT_EQ(regs.user_data(Stage::cs)[0], 0x55u);
}

TEST(RegisterMirror, ClearContextWritesTheDefaults) {
    RegisterMirror regs;
    const std::vector<std::uint32_t> all(0x400, 0xFF);
    regs.write(RegSpace::context, 0, all);
    regs.write(RegSpace::sh, 0, all);
    regs.clear_context();
    EXPECT_EQ(regs.get(0xA000), 0u);
    EXPECT_EQ(regs.get(0xA3FF), 0u);

    const std::array<std::uint32_t, 3> defaults = {1, 2, 3};
    regs.set_context_defaults(defaults);
    regs.write(RegSpace::context, 0, all);
    regs.clear_context();
    EXPECT_EQ(regs.get(0xA000), 1u);
    EXPECT_EQ(regs.get(0xA002), 3u);
    EXPECT_EQ(regs.get(0xA003), 0u);
    EXPECT_EQ(regs.get(0x2C00), 0xFFu);  // SH registers keep their values
}

}  // namespace
