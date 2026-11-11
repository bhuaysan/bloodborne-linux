// The translator derived from shadPS4 builds behind the host layer and runs from GCN code to
// SPIR-V that spirv-val accepts (issue #416).

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "support/corpus.hpp"
#include "support/spirv_val.hpp"
#include "shadps4/translate.hpp"
#include "shadps4_host/failure.hpp"
#include "support/game.hpp"

namespace {

using bb::test::shadps4::fragment_runtime_info;
using bb::test::shadps4::translate;

constexpr std::uint32_t s_endpgm = 0xbf810000;

// s_mov_b32 vcc_hi, #1 (every real program starts with it); v_mov_b32 v0, 0;
// exp mrt0 v0, v0, v0, v0 done vm; s_endpgm
const std::vector<std::uint32_t> constant_color = {
    0xbeeb03ff, 0x00000001, 0x7e000280, 0xf800180f, 0x00000000, s_endpgm,
};

// No instruction encoding starts with these bits, which the decoder asserts on.
const std::vector<std::uint32_t> illegal_encoding = {0xbeeb03ff, 0x00000001, 0xffffffff, s_endpgm};

TEST(ShaderTranslator, HandWrittenPixelShaderValidates) {
    if (!bb::test::gcn::have_spirv_val()) {
        GTEST_SKIP() << "spirv-val not found";
    }
    const auto module = translate(constant_color, fragment_runtime_info());
    ASSERT_FALSE(module.empty());
    EXPECT_EQ(bb::test::gcn::validate(module), "");
}

TEST(ShaderTranslator, AssertionFailureReachesTheCaller) {
    try {
        (void)translate(illegal_encoding, fragment_runtime_info());
        FAIL() << "the illegal encoding was translated";
    } catch (const bb::shader::TranslationFailure& failure) {
        const std::string what = failure.what();
        EXPECT_NE(what.find("decode.cpp"), std::string::npos) << what;
        EXPECT_NE(what.find("illegal encoding: 0xffffffff"), std::string::npos) << what;
    }
}

// s_wqm_b32 s0, s1 decodes but has no translation, so this fails halfway through translating,
// with the pools in use. The same pools then have to serve the next program.
TEST(ShaderTranslator, SamePoolsTranslateAfterAFailure) {
    const std::vector<std::uint32_t> untranslated = {0xbeeb03ff, 0x00000001, 0xbe800901, s_endpgm};
    Shader::Pools pools;
    EXPECT_THROW((void)translate(untranslated, fragment_runtime_info(), pools),
                 bb::shader::TranslationFailure);
    const auto module = translate(constant_color, fragment_runtime_info(), pools);
    ASSERT_FALSE(module.empty());
    if (bb::test::gcn::have_spirv_val()) {
        EXPECT_EQ(bb::test::gcn::validate(module), "");
    }
}

// The smallest pixel shader of the game's corpus that carries its register block.
TEST(ShaderTranslatorGame, SmallestPixelShaderValidates) {
    BB_REQUIRE_GAME(game);
    (void)game;
    if (!bb::test::gcn::have_spirv_val()) {
        GTEST_SKIP() << "spirv-val not found";
    }
    const auto root = bb::test::analysis_dir() / "shaders";
    BB_REQUIRE_GAME_DATA(root, "tools/shaderbnd");
    bb::test::gcn::Program smallest;
    for (const auto& file : bb::test::gcn::program_files(root)) {
        std::string why;
        auto program = bb::test::gcn::read_program(file, why);
        if (!program.code.empty() &&
            program.has_block &&
            program.type == static_cast<unsigned>(bb::test::gcn::ProgramType::ps) &&
            (smallest.code.empty() || program.code.size() < smallest.code.size())) {
            smallest = std::move(program);
        }
    }
    ASSERT_FALSE(smallest.code.empty());
    const auto module = translate(smallest.code, fragment_runtime_info(smallest.block));
    ASSERT_FALSE(module.empty());
    EXPECT_EQ(bb::test::gcn::validate(module), "");
    std::cout << "smallest pixel shader: " << smallest.code.size() << " dwords, "
              << module.size() << " dwords of SPIR-V\n";
}

}  // namespace
