// The translator without a device: every test program gives a module that spirv-val accepts, and
// what isn't supported throws GcnError naming it. The execution tests are in tests/gpu/runner/.

#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <set>
#include <string>
#include <string_view>
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

// A DS instruction: the opcode, the address VGPR, data0, data1, the 16-bit offset and the GDS bit.
std::vector<std::uint32_t> ds(unsigned op, unsigned vdst, unsigned addr, unsigned data0 = 0,
                              unsigned data1 = 0, unsigned offset = 0, bool gds = false) {
    return {0xd8000000u | op << 18 | unsigned{gds} << 17 | offset,
            vdst << 24 | data1 << 16 | data0 << 8 | addr};
}
constexpr unsigned ds_write_b32 = 13, ds_swizzle_b32 = 53, ds_read_b32 = 54, ds_add_u64 = 64;
const ShaderKey key5 = {.pgm_rsrc2 = 5u << 1 | 24u << 15};  // 5 user SGPRs, 12 KB of LDS

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
        if (name.starts_with("vs_") || name.starts_with("ps_")) {
            continue;  // the graphics programs, in stages_test.cpp
        }
        SCOPED_TRACE(name);
        const auto translation =
            translate(program_words(name), {}, {.pgm_rsrc2 = program_pgm_rsrc2(name)});
        EXPECT_EQ(validate(translation.spirv), "");
        // ballots and any(...) over the subgroup, for the SCC after a lane mask operation (also
        // s_wqm_b64 and s_not_b64 in salu_arith), and the LDS (whose accesses wait for the
        // subgroup), and the shuffles of ds_swizzle_b32
        EXPECT_EQ(translation.interface.wave_size_dependent,
                  name == "cross_lane" || name == "mask_exec" || name == "mask_scc" ||
                      name == "mask_branch" || name == "salu_arith" || name == "ds_swizzle" ||
                      name.starts_with("lds_"));
        // s[8:9] of cross_lane; the mask programs keep only lane masks in their mask pairs
        EXPECT_EQ(translation.interface.mask_round_trip, name == "cross_lane");
    }
}

// Ballots, v_readfirstlane_b32, v_readlane_b32, the branches on VCC and EXEC and LDS accesses make
// a shader depend on the wave size and declare maximal reconvergence; the lane-local v_mbcnt with
// a constant mask, v_writelane_b32, s_cmp and the branches on SCC don't.
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
    EXPECT_FALSE(dependent({sopc(4, 0, 1), sopp(5, 1), sopp(0)}));  // s_cmp_lt_i32; s_cbranch_scc1
    for (const unsigned op : {6, 7, 8, 9}) {  // s_cbranch_vccz, vccnz, execz, execnz
        EXPECT_TRUE(dependent({sopp(op, 1), sopp(0)})) << op;
    }
    EXPECT_TRUE(dependent(ds(ds_read_b32, 1, 0)));           // ds_read_b32 v1, v0
    EXPECT_TRUE(dependent(ds(ds_swizzle_b32, 1, 0, 0, 0, 0x041f)));  // ds_swizzle_b32 v1, v0
}

// A value written to VCC or an SGPR mask pair and read back as a value sets mask_round_trip, also
// when s_mov_b64, s_<op>_b64 or s_<op>_saveexec_b64 (through EXEC) moves it to the pair that is
// read, or when s_mov_b64 or s_<op>_b64 writes a constant other than 0 and -1; either one alone
// doesn't, a copy the other way doesn't, and neither does EXEC.
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

    // s10 = s0, s11 = s1
    const std::vector<std::uint32_t> values = {sop1(3, 10, 0), sop1(3, 11, 1)};
    const std::vector<std::uint32_t> copy = {sop1(4, 8, 10)};       // s_mov_b64 s[8:9], s[10:11]
    const std::vector<std::uint32_t> copy_back = {sop1(4, 10, 8)};  // s_mov_b64 s[10:11], s[8:9]
    const std::vector<std::uint32_t> read_s8 = {sop1(3, 6, 8)};     // s6 = s8
    EXPECT_TRUE(round_trip({cmp, values, copy, read_s8}));
    EXPECT_TRUE(round_trip({cmp, values, copy, copy_back, {sop1(4, 12, 8)}, {sop1(3, 6, 12)}}));
    EXPECT_FALSE(round_trip({cmp, values, copy_back, read_s8}));  // the value goes into s[10:11]
    EXPECT_TRUE(round_trip({cmp, {sop1(4, 8, 133)}, read_s8}));   // s_mov_b64 s[8:9], 5
    EXPECT_FALSE(round_trip({cmp, {sop1(4, 8, 128)}, read_s8}));  // s_mov_b64 s[8:9], 0
    EXPECT_FALSE(round_trip({cmp, {sop1(4, 8, 193)}, read_s8}));  // s_mov_b64 s[8:9], -1

    // The SALU bitwise family moves its sources' bits into its destination: both sources of
    // s_<op>_b64, and the source of s_<op>_saveexec_b64 through EXEC into a later saved EXEC.
    const std::vector<std::uint32_t> value_s8 = {sop1(3, 8, 0)};   // s8 = s0
    const std::vector<std::uint32_t> read_s10 = {sop1(3, 6, 10)};  // s6 = s10
    // s_and_b64 s[10:11] with s[8:9] or s[12:13] and -1
    EXPECT_TRUE(round_trip({value_s8, {sop2(15, 10, 8, 193)}, read_s10}));
    EXPECT_TRUE(round_trip({value_s8, {sop2(15, 10, 193, 8)}, read_s10}));
    EXPECT_FALSE(round_trip({value_s8, {sop2(15, 10, 12, 193)}, read_s10}));
    EXPECT_TRUE(round_trip({{sop2(17, 8, 10, 133)}, read_s8}));   // s_or_b64 s[8:9], s[10:11], 5
    EXPECT_FALSE(round_trip({{sop2(17, 8, 10, 193)}, read_s8}));  // s_or_b64 s[8:9], s[10:11], -1
    // s_and_saveexec_b64 s[10:11], s[8:9]
    const std::vector<std::uint32_t> saveexec = {sop1(36, 10, 8)};
    EXPECT_TRUE(round_trip({value_s8, saveexec, saveexec, read_s10}));
    EXPECT_TRUE(round_trip({value_s8, saveexec, {sop1(4, 12, 126)}, {sop1(3, 6, 12)}}));  // = exec
    EXPECT_FALSE(round_trip({cmp, saveexec, read_s10}));  // s[8:9] holds a lane mask
}

// The LDS accesses and barriers of a module in order: W a store to the LDS, R a load from it, S a
// barrier of subgroup execution scope and G one of workgroup scope.
std::string lds_events(const std::vector<std::uint32_t>& module) {
    std::map<std::uint32_t, std::uint32_t> constants;  // id -> value
    std::set<std::uint32_t> lds;                       // the LDS variable and pointers into it
    std::string events;
    for (std::size_t at = 5; at < module.size(); at += module[at] >> 16) {  // 5 header words
        const auto op = static_cast<spv::Op>(module[at] & 0xffff);
        const auto word = [&](std::size_t k) { return module[at + k]; };
        if (op == spv::Op::OpConstant) {
            constants[word(2)] = word(3);
        } else if (op == spv::Op::OpVariable &&
                   word(3) == static_cast<std::uint32_t>(spv::StorageClass::Workgroup)) {
            lds.insert(word(2));
        } else if (op == spv::Op::OpAccessChain && lds.contains(word(3))) {
            lds.insert(word(2));
        } else if (op == spv::Op::OpStore && lds.contains(word(1))) {
            events += 'W';
        } else if (op == spv::Op::OpLoad && lds.contains(word(3))) {
            events += 'R';
        } else if (op == spv::Op::OpControlBarrier) {
            const auto scope = static_cast<spv::Scope>(constants.at(word(1)));
            events += scope == spv::Scope::Subgroup    ? 'S'
                      : scope == spv::Scope::Workgroup ? 'G'
                                                       : '?';
        }
    }
    return events;
}

// Lanes of a wave see each other's earlier DS accesses with no s_barrier, so every DS instruction
// waits for the subgroup first (wave_sync). Lavapipe runs a subgroup in lockstep, so only the
// structure of the module tells: lds_wave writes, reads, writes and reads again.
TEST(GcnTranslate, DsInstructionsWaitForTheSubgroup) {
    EXPECT_EQ(lds_events(translate(program_words("lds_wave"), {}, key5).spirv), "SWSRSWSR");
}

// How many instructions of `op` the module has.
unsigned count_ops(const std::vector<std::uint32_t>& module, spv::Op op) {
    unsigned count = 0;
    for (std::size_t at = 5; at < module.size(); at += module[at] >> 16) {  // 5 header words
        count += (module[at] & 0xffff) == static_cast<std::uint32_t>(op);
    }
    return count;
}

// A ds_write_b8 or ds_write_b16 merges its bits into the dword with one compare-and-swap loop that
// starts from an atomic load. An atomic AND followed by an atomic OR would let another lane's
// write show between the two, and lavapipe runs the lanes in order, so only the structure of the
// module tells. lds_narrow has two of each.
TEST(GcnTranslate, SubDwordWritesAreOneCompareSwapLoop) {
    const auto count = [](spv::Op op) {
        return count_ops(translate(program_words("lds_narrow"), {}, key5).spirv, op);
    };
    EXPECT_EQ(count(spv::Op::OpAtomicLoad), 4u);
    EXPECT_EQ(count(spv::Op::OpAtomicCompareExchange), 4u);
    EXPECT_EQ(count(spv::Op::OpAtomicAnd), 0u);
    EXPECT_EQ(count(spv::Op::OpAtomicOr), 0u);
}

// The atomics that update their dword in a compare-and-swap loop (inc, dec, rsub, mskor) start
// from an atomic load: a plain one would race with the other lanes' atomics.
TEST(GcnTranslate, CompareSwapLoopsStartWithAnAtomicLoad) {
    const auto atomic_loads = [](std::string_view name) {
        return count_ops(translate(program_words(name), {}, key5).spirv, spv::Op::OpAtomicLoad);
    };
    // inc, dec, rsub (twice: the second runs in lane 0 alone) and mskor
    EXPECT_EQ(atomic_loads("lds_atomic"), 5u);
    EXPECT_EQ(atomic_loads("lds_rw"), 0u);
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
    EXPECT_EQ(error({vopc(0x21, 256, 1), s_endpgm}),  // v_cmp_lt_f64_e32 vcc, v[0:1], v[1:2]
              "unsupported instruction v_cmp_lt_f64_e32 at offset 0x0 (word 0x7c420300)");
    EXPECT_EQ(error({vop1(42, 1, 256), s_endpgm}),  // v_rcp_f32_e32 v1, v0
              "unsupported instruction v_rcp_f32_e32 at offset 0x0 (word 0x7e025500)");
    EXPECT_EQ(error({sop1(3, 0, 108), s_endpgm}),  // s_mov_b32 s0, tba_lo
              "unsupported register operand 108 of s_mov_b32 at offset 0x0 (word 0xbe80036c)");
    EXPECT_EQ(error({sop1(4, 126, 108), s_endpgm}),  // s_mov_b64 exec, tba
              "unsupported mask operand of s_mov_b64 at offset 0x0 (word 0xbefe046c)");
    EXPECT_EQ(error({sopk(18, 0, 1), s_endpgm}),  // s_getreg_b32 s0, hwreg(HW_REG_MODE)
              "unsupported instruction s_getreg_b32 at offset 0x0 (word 0xb9000001)");
    EXPECT_EQ(error({sop2(31, 0, 2, 3), s_endpgm}),  // s_lshl_b64 s[0:1], s[2:3], s3
              "unsupported instruction s_lshl_b64 at offset 0x0 (word 0x8f800302)");
    EXPECT_EQ(error({sopc(12, 0, 1), s_endpgm}),  // s_bitcmp0_b32 s0, s1
              "unsupported instruction s_bitcmp0_b32 at offset 0x0 (word 0xbf0c0100)");
    // v_cmp_lt_u32_e64 s[6:7], v0, v1 makes s[6:7] a mask
    const auto cmp = [](unsigned sdst, std::vector<std::uint32_t> rest) {
        auto code = vop3(0xc1, sdst, 256, 257);
        code.insert(code.end(), rest.begin(), rest.end());
        return code;
    };
    EXPECT_EQ(error({sop2(15, 2, 6, 126), s_endpgm}),  // s_and_b64 s[2:3], s[6:7], exec
              "user data s2 is used as a mask at offset 0x0 (word 0x87827e06)");
    EXPECT_EQ(error(cmp(2, {s_endpgm})),
              "user data s2 is used as a mask at offset 0x0 (word 0xd1820002)");
    EXPECT_EQ(error({s_endpgm}, {.pgm_rsrc2 = 1u << 7}),  // TGID_X_EN
              "COMPUTE_PGM_RSRC2 0x80 is not supported yet at offset 0x0 (word 0x00000000)");
    EXPECT_EQ(error({s_endpgm}, {.pgm_rsrc2 = 5u << 1 | 4u << 15}), "no error");  // LDS_SIZE
    const auto with_ds = [](std::vector<std::uint32_t> inst) {
        inst.push_back(s_endpgm);
        return inst;
    };
    EXPECT_EQ(error(with_ds(ds(ds_swizzle_b32, 1, 0, 0, 0, 0x8000))), "no error");
    EXPECT_EQ(error(with_ds(ds(ds_swizzle_b32, 1, 0, 0, 0, 0x8000)), {.pgm_rsrc2 = 5u << 1}),
              "no error");  // it does not use the LDS
    EXPECT_EQ(error(with_ds(ds(ds_swizzle_b32, 1, 0, 0, 0, 0x8000, true))),
              "GDS is not supported yet at offset 0x0 (word 0xd8d68000)");
    EXPECT_EQ(error(with_ds(ds(ds_add_u64, 0, 0, 2))),
              "unsupported instruction ds_add_u64 at offset 0x0 (word 0xd9000000)");
    EXPECT_EQ(error(with_ds(ds(ds_read_b32, 1, 0, 0, 0, 0, true))),
              "GDS is not supported yet at offset 0x0 (word 0xd8da0000)");
    // LDS_SIZE counts 128 dwords: none, the most (64 KB), and one unit more
    const auto lds = [](unsigned units) {
        return ShaderKey{.pgm_rsrc2 = 5u << 1 | units << 15};
    };
    const auto write = with_ds(ds(ds_write_b32, 0, 0, 1));
    EXPECT_EQ(error(write, lds(0)),
              "LDS_SIZE of 0 dwords is not supported at offset 0x0 (word 0xd8340000)");
    EXPECT_EQ(error(write, lds(129)),
              "LDS_SIZE of 16512 dwords is not supported at offset 0x0 (word 0xd8340000)");
    EXPECT_EQ(error(write, lds(128)), "no error");
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

TEST(GcnTranslate, BufferMemoryErrors) {
    const auto message = [](std::vector<std::uint32_t> code) {
        code.push_back(s_endpgm);
        return error(code);
    };
    EXPECT_TRUE(message(mubuf(8, 3, 1, 4, 128, 0, buf::offen))  // buffer_load_ubyte
                    .starts_with("unsupported instruction buffer_load_ubyte at offset"));
    EXPECT_TRUE(message(mubuf(12, 3, 1, 4, 128, 0, buf::offen | buf::tfe))
                    .starts_with("buffer_load_dword with tfe or lds is not supported yet at"));
    // addr64 takes no index or offset VGPR (Table 8.2); the decoder only sees the two address
    // VGPRs, so the flags are what tells.
    for (const auto flags : {buf::addr64 | buf::idxen, buf::addr64 | buf::offen}) {
        EXPECT_TRUE(message(mubuf(12, 3, 1, 4, 128, 0, flags))
                        .starts_with("buffer_load_dword with addr64 and idxen or offen is not "
                                     "allowed at"));
    }
}

// Typed loads: stores, tfe and the formats that have no conversion yet are errors that name them.
TEST(GcnTranslate, TypedMemoryErrors) {
    const auto message = [](unsigned op, unsigned dfmt, unsigned nfmt, std::uint64_t flags = 0) {
        auto code = mtbuf(op, 3, 1, 4, 128, 0, dfmt, nfmt, buf::offen | flags);
        code.push_back(s_endpgm);
        return error(code);
    };
    EXPECT_EQ(message(0, 4, 7), "no error");  // tbuffer_load_format_x, 32 bits, float
    EXPECT_EQ(message(0, 4, 4), "no error");
    EXPECT_TRUE(message(4, 4, 7).starts_with("unsupported instruction tbuffer_store_format_x at"));
    EXPECT_TRUE(message(0, 4, 7, buf::tfe)
                    .starts_with("tbuffer_load_format_x with tfe is not supported yet at"));
    EXPECT_TRUE(message(0, 4, 7, buf::addr64)  // the helper adds offen
                    .starts_with("tbuffer_load_format_x with addr64 and idxen or offen is not "
                                 "allowed at"));
    EXPECT_TRUE(
        message(0, 8, 4).starts_with("tbuffer_load_format_x: data format 8 is not supported"));
    EXPECT_TRUE(  // 10_11_11 waits for the small floats, whatever the number format
        message(0, 6, 4).starts_with("tbuffer_load_format_x: data format 6 is not supported"));
    EXPECT_TRUE(
        message(0, 0, 7).starts_with("tbuffer_load_format_x: data format 0 is not supported"));
    EXPECT_TRUE(
        message(0, 1, 7).starts_with("tbuffer_load_format_x: number format 7 of data format 1 "));
    EXPECT_TRUE(
        message(0, 4, 0).starts_with("tbuffer_load_format_x: number format 0 of data format 4 "));
}

// The float instructions take neg, abs, omod and clamp where the source or result is a float,
// and reject them elsewhere.
TEST(GcnTranslate, FloatModifiers) {
    const auto with = [](unsigned op, std::uint32_t low, std::uint32_t high) {
        auto code = vop3(op, 2, 256, 0, 0, low, high);
        code.push_back(s_endpgm);
        return code;
    };
    EXPECT_EQ(error(with(0x1a4, 0, neg(1))), "no error");  // v_floor_f32_e64 -v0
    EXPECT_EQ(error(with(0x188, abs_clamp(1), 0)), "no error");       // v_cvt_i32_f32_e64 |v0|
    EXPECT_EQ(error(with(0x188, abs_clamp(0, true), 0)),  // clamp on an integer result
              "modifiers of v_cvt_i32_f32_e64 are not supported yet at offset 0x0 "
              "(word 0xd3100802)");
    EXPECT_EQ(error(with(0x185, 0, neg(1))),  // -v0 on an integer source
              "modifiers of v_cvt_f32_i32_e64 are not supported yet at offset 0x0 "
              "(word 0xd30a0002)");
    EXPECT_EQ(error(with(0x185, abs_clamp(0, true), omod(2))), "no error");
}

// The execution modes of the key's float mode come with the first float instruction only.
TEST(GcnTranslate, FloatModes) {
    // The execution modes with a width literal (OpExecutionMode of 4 words; LocalSize has 6).
    const auto modes = [](const std::vector<std::uint32_t>& spirv) {
        std::vector<std::uint32_t> found;
        for (std::size_t i = 5; i < spirv.size(); i += spirv[i] >> 16) {
            if ((spirv[i] & 0xffff) == 16 && (spirv[i] >> 16) == 4) {
                found.push_back(spirv[i + 2]);
            }
        }
        return found;
    };
    constexpr std::uint32_t preserve = 4459, flush = 4460, nan = 4461, rte = 4462, rtz = 4463;
    const std::vector<std::uint32_t> add = {vop2(3, 1, 256, 2), s_endpgm};  // v_add_f32 v1, v0, v2
    const auto build = [&](std::uint32_t rsrc1, const std::vector<std::uint32_t>& code) {
        ShaderKey key = key5;
        key.pgm_rsrc1 = rsrc1;
        const auto spirv = translate(code, {}, key).spirv;
        if (have_spirv_val()) {
            EXPECT_EQ(validate(spirv), "");
        }
        return modes(spirv);
    };
    EXPECT_EQ(build(0, add), (std::vector<std::uint32_t>{nan, rte, flush}));
    EXPECT_EQ(build(3u << 16 | 3u << 12, add), (std::vector<std::uint32_t>{nan, rtz, preserve}));
    EXPECT_EQ(build(0, {s_endpgm}), std::vector<std::uint32_t>{});  // no float instruction
    // A typed load converts through f32 in every number format but UINT and SINT: a half float's
    // infinities, NaNs and -0 need the modes, an integer load needs none.
    const auto typed = [&](unsigned dfmt, unsigned nfmt) {
        auto code = mtbuf(0, 3, 1, 4, 128, 0, dfmt, nfmt, buf::offen);
        code.push_back(s_endpgm);
        return build(0, code);
    };
    const std::vector<std::uint32_t> float_modes = {nan, rte, flush};
    EXPECT_EQ(typed(5, 7), float_modes);  // 16_16, FLOAT
    EXPECT_EQ(typed(10, 0), float_modes);  // 8_8_8_8, UNORM
    EXPECT_EQ(typed(5, 1), float_modes);  // 16_16, SNORM
    EXPECT_EQ(typed(10, 3), float_modes);  // 8_8_8_8, SSCALED
    EXPECT_EQ(typed(5, 4), std::vector<std::uint32_t>{});  // 16_16, UINT
    EXPECT_EQ(typed(5, 5), std::vector<std::uint32_t>{});  // 16_16, SINT
    for (const std::uint32_t rsrc1 : {1u << 12, 2u << 12, 1u << 16, 2u << 16}) {
        EXPECT_EQ(error(add, {.pgm_rsrc1 = rsrc1, .pgm_rsrc2 = 5u << 1}),
                  std::format("PGM_RSRC1 {:#x}: this float mode is not supported at "
                              "offset 0x0 (word 0x00000000)", rsrc1));
    }
}

// GCN rounds the result of every float instruction and only v_fma_f32 fuses a multiply and an
// add, so the translated add, subtract and multiply (the mads, the omod multiply and the
// legacy multiply included) all carry NoContraction. float_alu and float_edge use them all.
TEST(GcnTranslate, FloatArithmeticIsNotContracted) {
    constexpr std::uint16_t op_decorate = 71, op_fadd = 129, op_fsub = 131, op_fmul = 133;
    constexpr std::uint32_t no_contraction = 42;
    for (const char* name : {"float_alu", "float_edge"}) {
        SCOPED_TRACE(name);
        const auto spirv = translate(program_words(name), {}, key5).spirv;
        std::set<std::uint32_t> decorated;
        std::size_t arithmetic = 0;
        for (std::size_t i = 5; i < spirv.size(); i += spirv[i] >> 16) {
            const std::uint16_t op = spirv[i] & 0xffff;
            if (op == op_decorate && spirv[i + 2] == no_contraction) {
                decorated.insert(spirv[i + 1]);
            }
        }
        for (std::size_t i = 5; i < spirv.size(); i += spirv[i] >> 16) {
            const std::uint16_t op = spirv[i] & 0xffff;
            if (op == op_fadd || op == op_fsub || op == op_fmul) {
                ++arithmetic;
                EXPECT_TRUE(decorated.contains(spirv[i + 2])) << "result id " << spirv[i + 2];
            }
        }
        EXPECT_GT(arithmetic, 10u);
    }
}

}  // namespace
