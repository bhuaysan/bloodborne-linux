// The translator without a device: every test program gives a module that spirv-val accepts, and
// what isn't supported throws GcnError naming it. The execution tests are in tests/gpu/runner/.

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "gpu/gcn/encode.hpp"
#include "gpu/gcn/gcn.hpp"
#include "gpu/gcn/programs.hpp"
#include "gpu/gcn/spirv_val.hpp"
#include "gpu/gcn/translate.hpp"

namespace {

using namespace bb::test::gcn;
using bb::gcn::GcnError;
using bb::gcn::ShaderKey;
using bb::gcn::translate;

constexpr std::uint32_t s_endpgm = sopp(1);
const ShaderKey key16 = {.pgm_rsrc2 = 16u << 1};  // 16 user SGPRs

TEST(GcnTranslate, ProgramsPassSpirvVal) {
    if (!have_spirv_val()) {
        GTEST_SKIP() << "spirv-val not found";
    }
    for (const auto& name : program_names()) {
        SCOPED_TRACE(name);
        const auto translation = translate(program_words(name), {}, key16);
        EXPECT_EQ(validate(translation.spirv), "");
        EXPECT_FALSE(translation.interface.wave_size_dependent);
    }
}

// Scalar ALU, a loop on SCC, and the vector ALU in VOP1, VOP2 and VOP3 encodings.
TEST(GcnTranslate, AluAndBranchesPassSpirvVal) {
    if (!have_spirv_val()) {
        GTEST_SKIP() << "spirv-val not found";
    }
    std::vector<std::uint32_t> code = {
        sop1(3, 5, 128),        // s_mov_b32 s5, 0
        sop2(0, 5, 5, 2),       // s_add_u32 s5, s5, s2
        sop2(1, 2, 2, 129),     // s_sub_u32 s2, s2, 1
        sopp(4, 0xfffd),        // s_cbranch_scc0 -3
        sop1(4, 8, 193),        // s_mov_b64 s[8:9], -1
        vop2(0x1a, 1, 130, 0),  // v_lshlrev_b32_e32 v1, 2, v0
    };
    for (const auto& words : {vop3(0x181, 2, 8),             // v_mov_b32_e64 v2, s8
                              vop3(0x11b, 3, 256 + 1, 5)}) {  // v_and_b32_e64 v3, v1, s5
        code.insert(code.end(), words.begin(), words.end());
    }
    code.push_back(s_endpgm);
    EXPECT_EQ(validate(translate(code, {}, key16).spirv), "");
}

// The message of the GcnError that translating `code` throws.
std::string error(const std::vector<std::uint32_t>& code, const ShaderKey& key = key16) {
    try {
        translate(code, {}, key);
    } catch (const GcnError& e) {
        return e.what();
    }
    return "no error";
}

TEST(GcnTranslate, ErrorsNameWhatIsUnsupported) {
    EXPECT_EQ(error({vopc(0xc1, 256, 1), s_endpgm}),  // v_cmp_lt_u32_e32 vcc, v0, v1
              "unsupported instruction v_cmp_lt_u32_e32 at offset 0x0 (word 0x7d820300)");
    EXPECT_EQ(error({sop1(3, 0, 126), s_endpgm}),  // s_mov_b32 s0, exec_lo
              "exec_lo is read as a value at offset 0x0 (word 0xbe80037e)");
    EXPECT_EQ(error({sop1(3, 0, 106), s_endpgm}),  // s_mov_b32 s0, vcc_lo
              "unsupported register operand 106 of s_mov_b32 at offset 0x0 (word 0xbe80036a)");
    EXPECT_EQ(error({s_endpgm}, {.stage = bb::gcn::Stage::pixel}),
              "only the compute stage is translated so far at offset 0x0 (word 0x00000000)");
    EXPECT_EQ(error({sop1(4, 126, 0), s_endpgm}),  // s_mov_b64 exec, s[0:1]
              "unsupported mask operand of s_mov_b64 at offset 0x0 (word 0xbefe0400)");
    EXPECT_EQ(error({s_endpgm}, {.pgm_rsrc2 = 1u << 7}),  // TGID_X_EN
              "COMPUTE_PGM_RSRC2 0x80 is not supported yet at offset 0x0 (word 0x00000000)");
    EXPECT_EQ(error({s_endpgm}, {.pgm_rsrc2 = 16u << 1 | 4u << 15}), "no error");  // LDS_SIZE
    EXPECT_EQ(error({sop1(4, 0, 255), 1, s_endpgm}),  // s_mov_b64 s[0:1], 1 (a literal)
              "64-bit literals are not supported yet at offset 0x0 (word 0xbe8004ff)");
    EXPECT_EQ(error({s_endpgm}, {.pgm_rsrc2 = 16u << 1, .num_threads = {0, 1, 1}}),
              "a workgroup size of 0 at offset 0x0 (word 0x00000000)");
}

// Ignoring a modifier would give wrong results without failing validation, so each one throws.
TEST(GcnTranslate, VectorModifiersAreRejected) {
    const auto with = [](std::uint32_t low, std::uint32_t high) {  // v_mov_b32_e64 v2, s8
        auto code = vop3(0x181, 2, 8, 0, 0, low, high);
        code.push_back(s_endpgm);
        return code;
    };
    EXPECT_EQ(error(with(0, neg(1))),
              "modifiers of v_mov_b32_e64 are not supported yet at offset 0x0 (word 0xd3020002)");
    EXPECT_EQ(error(with(abs_clamp(1), 0)),
              "modifiers of v_mov_b32_e64 are not supported yet at offset 0x0 (word 0xd3020102)");
    EXPECT_EQ(error(with(abs_clamp(0, true), 0)),
              "modifiers of v_mov_b32_e64 are not supported yet at offset 0x0 (word 0xd3020802)");
    EXPECT_EQ(error(with(0, omod(1))),
              "modifiers of v_mov_b32_e64 are not supported yet at offset 0x0 (word 0xd3020002)");
    EXPECT_EQ(error(with(0, 0)), "no error");
}

}  // namespace
