// Control flow graph tests on synthetic programs: SOPP branches and placeholder instructions,
// encoded by hand from the Sea Islands ISA manual.

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "gpu/gcn/cfg.hpp"
#include "gpu/gcn/encode.hpp"

namespace {

using namespace bb::gcn;
using bb::test::gcn::sopp;

// SOPP opcodes.
constexpr unsigned nop = 0, endpgm = 1, branch = 2, scc0 = 4, scc1 = 5, vccz = 6, vccnz = 7,
                   execz = 8, execnz = 9;

// A SOPP branch at instruction `at` to instruction `to`, all instructions one dword long.
std::uint32_t br(unsigned op, int at, int to) {
    return sopp(op, static_cast<unsigned>(to - at - 1));
}

std::vector<Instruction> program(const std::vector<std::uint32_t>& words) {
    return decode(words, Until::code_end);
}

// Turns the placeholder at `index` into a scalar ALU instruction (SOP1 isn't decoded yet).
void write_scalar(std::vector<Instruction>& program, std::size_t index) {
    Instruction& inst = program.at(index);
    inst.format = Format::sop1;
    inst.name = "s_mov_b32";
    inst.num_dst = 1;
    inst.dst[0] = {OperandKind::scalar, 1, 0, 0};
}

std::string error(const std::vector<Instruction>& program) {
    try {
        build_cfg(program);
    } catch (const GcnError& e) {
        return e.what();
    }
    return "no GcnError";
}

TEST(GcnCfg, SplitsAtBranchesAndTargets) {
    // 0: if scc0 goto 3; 1: nop; 2: goto 4; 3: nop; 4: end
    const auto p = program({br(scc0, 0, 3), sopp(nop), br(branch, 2, 4), sopp(nop), sopp(endpgm)});
    const Cfg cfg = build_cfg(p);
    ASSERT_EQ(cfg.blocks.size(), 4u);
    const Block& entry = cfg.blocks[0];
    EXPECT_EQ(entry.exit, Exit::conditional);
    EXPECT_EQ(successors(entry), (std::vector<std::uint32_t>{2, 1}));
    EXPECT_EQ(cfg.blocks[1].first, 1u);
    EXPECT_EQ(cfg.blocks[1].count, 2u);
    EXPECT_EQ(cfg.blocks[1].exit, Exit::jump);
    EXPECT_EQ(successors(cfg.blocks[1]), (std::vector<std::uint32_t>{3}));
    EXPECT_EQ(cfg.blocks[2].offset, 12u);
    EXPECT_EQ(cfg.blocks[2].exit, Exit::next);
    EXPECT_EQ(successors(cfg.blocks[2]), (std::vector<std::uint32_t>{3}));
    EXPECT_EQ(cfg.blocks[3].exit, Exit::end);
    EXPECT_TRUE(successors(cfg.blocks[3]).empty());
}

TEST(GcnCfg, LoopsAndBranchesToTheNextInstruction) {
    // 0: nop; 1: if execnz goto 0; 2: if scc0 goto 3; 3: end
    const auto p = program({sopp(nop), br(execnz, 1, 0), br(scc0, 2, 3), sopp(endpgm)});
    const Cfg cfg = build_cfg(p);
    ASSERT_EQ(cfg.blocks.size(), 3u);
    EXPECT_EQ(successors(cfg.blocks[0]), (std::vector<std::uint32_t>{0, 1}));
    EXPECT_EQ(cfg.blocks[1].exit, Exit::next);  // both edges go to the same block
    EXPECT_EQ(successors(cfg.blocks[1]), (std::vector<std::uint32_t>{2}));
}

TEST(GcnCfg, RejectsBadTargetsAndUnsupportedBranches) {
    // A literal makes instruction 1 two dwords long; a branch to its literal misses.
    auto p = program({br(scc0, 0, 2), sopp(nop), sopp(nop), sopp(endpgm)});
    p[1].size = 2;
    p[2].offset = 12;
    p[3].offset = 16;
    EXPECT_NE(error(p).find("branch target 0x8 is not an instruction"), std::string::npos)
        << error(p);
    EXPECT_NE(error(p).find("at offset 0x0"), std::string::npos);
    EXPECT_NE(error(program({br(branch, 0, 2), sopp(endpgm)})).find("not an instruction"),
              std::string::npos);
    EXPECT_NE(error(program({sopp(nop), br(execz, 1, 0)})).find("runs off the end"),
              std::string::npos);
    EXPECT_NE(error(program({sopp(23, 0), sopp(endpgm)})).find("unsupported branch"),
              std::string::npos);  // s_cbranch_cdbgsys
    p = program({sopp(nop), sopp(endpgm)});
    p[0].format = Format::sop1;
    p[0].name = "s_setpc_b64";
    EXPECT_NE(error(p).find("unsupported branch s_setpc_b64"), std::string::npos);
    EXPECT_NE(error({}).find("empty program"), std::string::npos);
}

// 0: if `op` goto 3; 1: s_mov_b32 (or nop); 2: nop; 3: end
Cfg skip(unsigned op, bool scalar) {
    auto p = program({br(op, 0, 3), sopp(nop), sopp(nop), sopp(endpgm)});
    if (scalar) {
        write_scalar(p, 1);
    }
    return build_cfg(p);
}

TEST(GcnCfg, ExeczSkipOverScalarWritesFallsThrough) {
    const Cfg cfg = skip(execz, true);
    ASSERT_EQ(cfg.blocks.size(), 3u);
    EXPECT_EQ(cfg.blocks[0].exit, Exit::next);
    EXPECT_EQ(successors(cfg.blocks[0]), (std::vector<std::uint32_t>{1}));
    EXPECT_EQ(cfg.blocks[0].taken, 2u);  // still recorded
    // Without scalar writes the skip stays a branch, and vccz and vccnz always do.
    EXPECT_EQ(skip(execz, false).blocks[0].exit, Exit::conditional);
    EXPECT_EQ(skip(vccz, true).blocks[0].exit, Exit::conditional);
    EXPECT_EQ(skip(vccnz, true).blocks[0].exit, Exit::conditional);
}

TEST(GcnCfg, ExeczSkipStaysABranchWhenTheRegionIsOpen) {
    // A loop exit: 0: nop; 1: if execz goto 4; 2: s_mov_b32; 3: goto 0; 4: end
    auto p = program({sopp(nop), br(execz, 1, 4), sopp(nop), br(branch, 3, 0), sopp(endpgm)});
    write_scalar(p, 2);
    EXPECT_EQ(build_cfg(p).blocks[0].exit, Exit::conditional);
    // A second entry: 0: if scc0 goto 2; 1: if execz goto 3; 2: s_mov_b32; 3: end
    p = program({br(scc0, 0, 2), br(execz, 1, 3), sopp(nop), sopp(endpgm)});
    write_scalar(p, 2);
    EXPECT_EQ(build_cfg(p).blocks[1].exit, Exit::conditional);
    // The region ends the program: 0: if execz goto 3; 1: s_mov_b32; 2: end; 3: end
    p = program({br(execz, 0, 3), sopp(nop), sopp(endpgm), sopp(endpgm)});
    write_scalar(p, 1);
    EXPECT_EQ(build_cfg(p).blocks[0].exit, Exit::conditional);
}

TEST(GcnCfg, NestedExeczSkipsFallThrough) {
    // 0: if execz goto 4; 1: if execz goto 3; 2: s_mov_b32; 3: nop; 4: end
    auto p = program({br(execz, 0, 4), br(execz, 1, 3), sopp(nop), sopp(nop), sopp(endpgm)});
    write_scalar(p, 2);
    const Cfg cfg = build_cfg(p);
    EXPECT_EQ(cfg.blocks[0].exit, Exit::next);
    EXPECT_EQ(cfg.blocks[1].exit, Exit::next);
}

TEST(GcnCfg, AnalyzesOrderDominatorsAndLoops) {
    constexpr std::uint32_t none = CfgAnalysis::none;
    // If/else: blocks 0 [0], 1 [1-2], 2 [3], 3 [4]; block 3 joins.
    CfgAnalysis a = analyze(build_cfg(
        program({br(scc0, 0, 3), sopp(nop), br(branch, 2, 4), sopp(nop), sopp(endpgm)})));
    EXPECT_TRUE(a.reducible);
    EXPECT_EQ(a.rpo, (std::vector<std::uint32_t>{0, 1, 2, 3}));
    EXPECT_EQ(a.idom, (std::vector<std::uint32_t>{0, 0, 0, 0}));
    EXPECT_EQ(a.merge_node, (std::vector<bool>{false, false, false, true}));
    EXPECT_EQ(a.loop_header, (std::vector<bool>(4, false)));
    EXPECT_TRUE(a.dominates(0, 3));
    EXPECT_FALSE(a.dominates(1, 3));

    // Nested loops and an unreachable block: 0: nop; 1: nop; 2: if scc1 goto 2;
    // 3: if vccz goto 1; 4: end; 5: nop (unreachable); 6: end
    a = analyze(build_cfg(program({sopp(nop), sopp(nop), br(scc1, 2, 2), br(vccz, 3, 1),
                                   sopp(endpgm), sopp(nop), sopp(endpgm)})));
    EXPECT_TRUE(a.reducible);
    EXPECT_EQ(a.rpo, (std::vector<std::uint32_t>{0, 1, 2, 3, 4}));
    EXPECT_EQ(a.order[5], none);
    EXPECT_EQ(a.idom, (std::vector<std::uint32_t>{0, 0, 1, 2, 3, none}));
    EXPECT_EQ(a.loop_header, (std::vector<bool>{false, true, true, false, false, false}));
    EXPECT_EQ(a.merge_node, (std::vector<bool>(6, false)));
    EXPECT_TRUE(a.dominates(1, 4));
    EXPECT_TRUE(a.dominates(2, 2));
    EXPECT_FALSE(a.dominates(3, 2));

    // A loop with two entries: 0: if scc0 goto 2; 1: nop; 2: if scc1 goto 1; 3: end
    a = analyze(build_cfg(program({br(scc0, 0, 2), sopp(nop), br(scc1, 2, 1), sopp(endpgm)})));
    EXPECT_FALSE(a.reducible);
    EXPECT_EQ(a.idom[2], 0u);
}

}  // namespace
