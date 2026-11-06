// The translator without a device: every test program gives a module that spirv-val accepts, and
// what isn't supported throws GcnError naming it. The execution tests are in tests/gpu/runner/.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <spirv/unified1/spirv.hpp11>

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
const ShaderKey key5 = {.pgm_rsrc2 = 5u << 1};  // 5 user SGPRs, s5 and up for masks

// Whether the module declares the execution mode MaximallyReconvergesKHR.
bool maximally_reconverges(const std::vector<std::uint32_t>& spirv) {
    for (std::size_t i = 5; i < spirv.size() && (spirv[i] >> 16) != 0; i += spirv[i] >> 16) {
        if ((spirv[i] & 0xffff) == static_cast<std::uint32_t>(spv::Op::OpExecutionMode) &&
            i + 2 < spirv.size() &&
            spirv[i + 2] ==
                static_cast<std::uint32_t>(spv::ExecutionMode::MaximallyReconvergesKHR)) {
            return true;
        }
    }
    return false;
}

TEST(GcnTranslate, ProgramsPassSpirvVal) {
    if (!have_spirv_val()) {
        GTEST_SKIP() << "spirv-val not found";
    }
    for (const auto& name : program_names()) {
        SCOPED_TRACE(name);
        const auto translation = translate(program_words(name), {}, key5);
        EXPECT_EQ(validate(translation.spirv), "");
        EXPECT_EQ(translation.interface.wave_size_dependent, name == "cross_lane");
        EXPECT_EQ(translation.interface.mask_round_trip, name == "cross_lane");  // s[8:9]
    }
}

// Ballots, v_readfirstlane_b32 and v_readlane_b32 make a shader depend on the wave size and
// declare maximal reconvergence; the lane-local v_mbcnt with a constant mask and v_writelane_b32
// don't.
TEST(GcnTranslate, WaveSizeDependence) {
    const auto dependent = [](std::vector<std::uint32_t> code) {
        code.push_back(s_endpgm);
        const auto translation = translate(code, {}, key5);
        if (have_spirv_val()) {
            EXPECT_EQ(validate(translation.spirv), "");
        }
        EXPECT_EQ(maximally_reconverges(translation.spirv),
                  translation.interface.wave_size_dependent);
        return translation.interface.wave_size_dependent;
    };
    EXPECT_FALSE(dependent(vop3(0x123, 1, 193, 128)));       // v_mbcnt_lo_u32_b32_e64 v1, -1, 0
    EXPECT_FALSE(dependent({vop2(0x02, 1, 0, 131)}));        // v_writelane_b32 v1, s0, 3
    EXPECT_TRUE(dependent({sop1(16, 6, 126)}));              // s_bcnt1_i32_b64 s6, exec
    EXPECT_TRUE(dependent({vop2(0x01, 6, 256 + 1, 131)}));   // v_readlane_b32 s6, v1, 3
    EXPECT_TRUE(dependent({vop1(0x02, 6, 256 + 1)}));        // v_readfirstlane_b32 s6, v1
    EXPECT_TRUE(dependent(vop3(0x123, 1, 106, 128)));        // v_mbcnt_lo_u32_b32_e64 v1, vcc_lo, 0
}

// A value written to VCC or an SGPR mask pair and read back as a value sets mask_round_trip;
// either one alone doesn't, and neither does EXEC.
TEST(GcnTranslate, MaskRoundTrip) {
    const auto round_trip = [](const std::vector<std::vector<std::uint32_t>>& parts) {
        std::vector<std::uint32_t> code;
        for (const auto& part : parts) {
            code.insert(code.end(), part.begin(), part.end());
        }
        code.push_back(s_endpgm);
        return translate(code, {}, key5).interface.mask_round_trip;
    };
    const auto cmp = vop3(0xc1, 8, 256, 257);  // v_cmp_lt_u32_e64 s[8:9], v0, v1
    EXPECT_TRUE(round_trip({cmp, {sop1(3, 8, 0)}, {sop1(3, 6, 9)}}));  // s8 = s0, s6 = s9
    EXPECT_FALSE(round_trip({cmp, {sop1(3, 6, 9)}}));                   // s6 = s9
    EXPECT_FALSE(round_trip({cmp, {sop1(3, 8, 0)}}));                   // s8 = s0
    EXPECT_TRUE(round_trip({{sop1(3, 106, 0)}, {sop1(3, 6, 107)}}));    // vcc_lo = s0, s6 = vcc_hi
    EXPECT_FALSE(round_trip({{sop1(3, 126, 0)}, {sop1(3, 6, 126)}}));   // exec_lo = s0, s6 = exec_lo
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
    EXPECT_EQ(validate(translate(code, {}, key5).spirv), "");
}

// The message of the GcnError that translating `code` throws.
std::string error(const std::vector<std::uint32_t>& code, const ShaderKey& key = key5) {
    try {
        translate(code, {}, key);
    } catch (const GcnError& e) {
        return e.what();
    }
    return "no error";
}

TEST(GcnTranslate, ErrorsNameWhatIsUnsupported) {
    EXPECT_EQ(error({vopc(0x01, 256, 1), s_endpgm}),  // v_cmp_lt_f32_e32 vcc, v0, v1
              "unsupported instruction v_cmp_lt_f32_e32 at offset 0x0 (word 0x7c020300)");
    EXPECT_EQ(error({sop1(3, 0, 108), s_endpgm}),  // s_mov_b32 s0, tba_lo
              "unsupported register operand 108 of s_mov_b32 at offset 0x0 (word 0xbe80036c)");
    EXPECT_EQ(error({s_endpgm}, {.stage = bb::gcn::Stage::pixel}),
              "only the compute stage is translated so far at offset 0x0 (word 0x00000000)");
    EXPECT_EQ(error({sop1(4, 126, 108), s_endpgm}),  // s_mov_b64 exec, tba
              "unsupported mask operand of s_mov_b64 at offset 0x0 (word 0xbefe046c)");
    EXPECT_EQ(error({sopc(0, 0, 1), s_endpgm}),  // s_cmp_eq_i32 s0, s1
              "unsupported instruction s_cmp_eq_i32 at offset 0x0 (word 0xbf000100)");
    EXPECT_EQ(error({sopp(6, 1), sopp(0), s_endpgm}),  // s_cbranch_vccz 1
              "unsupported instruction s_cbranch_vccz at offset 0x0 (word 0xbf860001)");
    // v_cmp_lt_u32_e64 s[6:7], v0, v1 makes s[6:7] a mask
    const auto cmp = [](unsigned sdst, std::vector<std::uint32_t> rest) {
        auto code = vop3(0xc1, sdst, 256, 257);
        code.insert(code.end(), rest.begin(), rest.end());
        return code;
    };
    EXPECT_EQ(error(cmp(2, {s_endpgm})),
              "user data s2 is used as a mask at offset 0x0 (word 0xd1820002)");
    EXPECT_EQ(error({s_endpgm}, {.pgm_rsrc2 = 1u << 7}),  // TGID_X_EN
              "COMPUTE_PGM_RSRC2 0x80 is not supported yet at offset 0x0 (word 0x00000000)");
    EXPECT_EQ(error({s_endpgm}, {.pgm_rsrc2 = 5u << 1 | 4u << 15}), "no error");  // LDS_SIZE
    EXPECT_EQ(error({sop1(4, 0, 255), 1, s_endpgm}),  // s_mov_b64 s[0:1], 1 (a literal)
              "64-bit literals are not supported yet at offset 0x0 (word 0xbe8004ff)");
    EXPECT_EQ(error({s_endpgm}, {.pgm_rsrc2 = 5u << 1, .num_threads = {0, 1, 1}}),
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
