// Structurizer tests on synthetic programs. Each module must pass spirv-val, and a small
// interpreter for the instructions the structurizer emits runs it: the blocks it visits must be
// the ones a walk over the graph visits, with the same branch outcomes.

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "gpu/gcn/structurize.hpp"
#include "gpu/gcn/encode.hpp"
#include "support/spirv_val.hpp"

namespace {

using namespace bb::gcn;
using bb::test::gcn::have_spirv_val;
using bb::test::gcn::sopp;
using bb::test::gcn::validate;
using Id = SpirvBuilder::Id;
using Op = spv::Op;
// The outcomes of each block's branch condition, in the order it is evaluated; false after them.
using Script = std::map<std::uint32_t, std::vector<bool>>;

constexpr unsigned nop = 0, endpgm = 1, branch = 2, scc0 = 4, scc1 = 5, vccz = 6, vccnz = 7,
                   execz = 8;

std::uint32_t br(unsigned op, int at, int to) {
    return sopp(op, static_cast<unsigned>(to - at - 1));
}

// A compute module whose blocks store their index to `trace` and load their condition from
// `flag`. Counts how often each block is emitted.
struct Module {
    std::vector<std::uint32_t> words;
    Id trace = 0, flag = 0;
    std::map<std::uint32_t, int> emitted;
};

Module build(const Cfg& cfg) {
    Module m;
    SpirvBuilder b;
    b.capability(spv::Capability::Shader);
    b.memory_model(spv::AddressingModel::Logical, spv::MemoryModel::GLSL450);
    const Id void_t = b.type_void();
    const Id u32 = b.type_int(32, false);
    const Id bool_t = b.type_bool();
    m.trace = b.variable(b.type_pointer(spv::StorageClass::Private, u32),
                         spv::StorageClass::Private);
    m.flag = b.variable(b.type_pointer(spv::StorageClass::Private, bool_t),
                        spv::StorageClass::Private);
    const Id main = b.begin_function(void_t, b.type_function(void_t));
    b.begin_block(b.id());
    const BlockEmitter emitter = {
        .body = [&](std::uint32_t block) {
            ++m.emitted[block];
            b.op_void(Op::OpStore, {m.trace, b.constant_u32(block)});
        },
        .condition = [&](std::uint32_t) { return b.op(Op::OpLoad, bool_t, {m.flag}); },
    };
    structurize(cfg, b, emitter);
    b.end_function();
    b.entry_point(spv::ExecutionModel::GLCompute, main, "main", {m.trace, m.flag});
    b.execution_mode(main, spv::ExecutionMode::LocalSize, {1, 1, 1});
    m.words = b.words();
    return m;
}

bool next_outcome(Script& script, std::uint32_t block) {
    auto& outcomes = script[block];
    if (outcomes.empty()) {
        return false;
    }
    const bool outcome = outcomes.front();
    outcomes.erase(outcomes.begin());
    return outcome;
}

// The blocks a walk over the graph visits.
std::vector<std::uint32_t> walk(const Cfg& cfg, Script script) {
    std::vector<std::uint32_t> trace;
    for (std::uint32_t block = 0; trace.size() < 1000;) {
        trace.push_back(block);
        const Block& b = cfg.blocks[block];
        if (b.exit == Exit::end) {
            break;
        }
        const bool taken = b.exit == Exit::jump ||
                           (b.exit == Exit::conditional && next_outcome(script, block));
        block = taken ? b.taken : b.next;
    }
    return trace;
}

// Runs the module's function: constants, variables, loads, stores, OpIEqual, OpSelect and the
// branches, OpSwitch among them.
std::vector<std::uint32_t> run(const Module& m, Script script) {
    std::map<Id, std::uint32_t> values, memory;
    std::map<Id, std::size_t> labels;
    std::size_t pc = 0;
    for (std::size_t i = 5; i < m.words.size(); i += m.words[i] >> 16) {
        const auto op = static_cast<Op>(m.words[i] & 0xffff);
        if (op == Op::OpConstant) {
            values[m.words[i + 2]] = m.words[i + 3];
        } else if (op == Op::OpLabel) {
            labels[m.words[i + 1]] = i;
            pc = pc == 0 ? i : pc;
        }
    }
    std::vector<std::uint32_t> trace;
    for (int steps = 0; steps < 100000; ++steps) {
        const std::uint32_t* w = &m.words[pc];
        pc += w[0] >> 16;
        switch (static_cast<Op>(w[0] & 0xffff)) {
        case Op::OpLabel:
        case Op::OpSelectionMerge:
        case Op::OpLoopMerge:
            break;
        case Op::OpVariable:
            memory[w[2]] = (w[0] >> 16) > 4 ? values.at(w[4]) : 0;
            break;
        case Op::OpStore:
            if (w[1] == m.trace) {
                trace.push_back(values.at(w[2]));
            } else {
                memory[w[1]] = values.at(w[2]);
            }
            break;
        case Op::OpLoad:
            values[w[2]] = w[3] == m.flag ? next_outcome(script, trace.back()) : memory.at(w[3]);
            break;
        case Op::OpIEqual:
            values[w[2]] = values.at(w[3]) == values.at(w[4]);
            break;
        case Op::OpSelect:
            values[w[2]] = values.at(values.at(w[3]) != 0 ? w[4] : w[5]);
            break;
        case Op::OpBranch:
            pc = labels.at(w[1]);
            break;
        case Op::OpBranchConditional:
            pc = labels.at(values.at(w[1]) != 0 ? w[2] : w[3]);
            break;
        case Op::OpSwitch:
            pc = labels.at(w[2]);
            for (std::uint32_t k = 3; k + 1 < (w[0] >> 16); k += 2) {
                if (w[k] == values.at(w[1])) {
                    pc = labels.at(w[k + 1]);
                }
            }
            break;
        case Op::OpReturn:
            return trace;
        default:
            throw std::runtime_error("unexpected opcode " + std::to_string(w[0] & 0xffff));
        }
    }
    throw std::runtime_error("no return");
}

// Structurizes the program, validates the module, and runs it with each script.
void check(const std::vector<std::uint32_t>& words, const std::vector<Script>& scripts) {
    const Cfg cfg = build_cfg(decode(words, Until::code_end));
    const Module m = build(cfg);
    for (const auto& [block, count] : m.emitted) {
        EXPECT_EQ(count, 1) << "block " << block;
    }
    if (have_spirv_val()) {
        EXPECT_EQ(validate(m.words), "");
    }
    for (const Script& script : scripts) {
        EXPECT_EQ(run(m, script), walk(cfg, script));
    }
}

TEST(GcnStructurize, IfElse) {
    // 0: if scc0 goto 3; 1: nop; 2: goto 4; 3: nop; 4: end
    check({br(scc0, 0, 3), sopp(nop), br(branch, 2, 4), sopp(nop), sopp(endpgm)},
          {{{0, {true}}}, {{0, {false}}}});
}

TEST(GcnStructurize, NestedLoops) {
    // 0: nop; 1: nop; 2: if scc1 goto 2; 3: if vccnz goto 1; 4: end
    // Blocks: 0 entry, 1 outer header, 2 inner loop, 3 outer latch, 4 exit.
    check({sopp(nop), sopp(nop), br(scc1, 2, 2), br(vccnz, 3, 1), sopp(endpgm)},
          {{{2, {true, true, false, true, false}}, {3, {true, false}}}, {}});
}

TEST(GcnStructurize, EarlyExits) {
    // 0: if scc0 goto 7 (join); 1: if execz goto 7, out of the loop; 2: if vccz goto 6 (return);
    // 3: if scc1 goto 1 (back edge); 4: goto 7; 5: unreachable; 6: end; 7: nop; 8: end
    const std::vector<std::uint32_t> words = {
        br(scc0, 0, 7), br(execz, 1, 7), br(vccz, 2, 6), br(scc1, 3, 1),
        br(branch, 4, 7), sopp(nop), sopp(endpgm), sopp(nop), sopp(endpgm)};
    // Block i holds instruction i, block 7 instructions 7 and 8. Block 5 is unreachable.
    check(words, {{{0, {true}}},
                  {{1, {false, true}}, {3, {true}}},
                  {{2, {false, true}}, {3, {true}}},
                  {{3, {true, true}}}});
}

TEST(GcnStructurize, ContinueFromInsideAScope) {
    // 0: nop; 1: if scc0 goto 4; 2: if scc1 goto 1; 3: nop; 4: if vccnz goto 1; 5: end
    // Block 4 is a merge node inside the loop, and block 2 continues the loop from its scope.
    check({sopp(nop), br(scc0, 1, 4), br(scc1, 2, 1), sopp(nop), br(vccnz, 4, 1), sopp(endpgm)},
          {{{1, {false, false, true}}, {2, {true, false}}, {4, {true}}}, {}});
}

TEST(GcnStructurize, ExitsOutOfNestedLoops) {
    // 0: if vccz goto 5; 1: nop; 2: if scc0 goto 5; 3: if scc1 goto 1; 4: if vccnz goto 2; 5: end
    // Block 3 continues the outer loop from the inner one, block 2 leaves both to the merge 5.
    check({br(vccz, 0, 5), sopp(nop), br(scc0, 2, 5), br(scc1, 3, 1), br(vccnz, 4, 2),
           sopp(endpgm)},
          {{{0, {true}}}, {{2, {false, false, true}}, {3, {true, false}}, {4, {true}}}, {}});
}

TEST(GcnStructurize, IrreducibleGraphs) {
    // 0: if scc0 goto 2; 1: nop; 2: if scc1 goto 1; 3: end. The loop {1, 2} has two entries.
    const std::vector<std::uint32_t> two_entries = {br(scc0, 0, 2), sopp(nop), br(scc1, 2, 1),
                                                    sopp(endpgm)};
    // The same inside a natural loop: 0: nop; 1: if scc0 goto 3; 2: nop; 3: if scc1 goto 2;
    // 4: if vccnz goto 1; 5: end. Block i holds instruction i.
    const std::vector<std::uint32_t> in_loop = {sopp(nop),      br(scc0, 1, 3),  sopp(nop),
                                                br(scc1, 3, 2), br(vccnz, 4, 1), sopp(endpgm)};
    for (const auto& words : {two_entries, in_loop}) {
        EXPECT_FALSE(analyze(build_cfg(decode(words, Until::code_end))).reducible);
    }
    check(two_entries, {{{0, {true}}, {2, {true, true}}}, {{2, {true}}}, {}});
    check(in_loop,
          {{{1, {true, false}}, {3, {true, false, true}}, {4, {true}}}, {{4, {true, true}}}, {}});
}

TEST(GcnStructurize, RandomGraphs) {
    // Random programs of nops, s_endpgm, forward s_branch and conditional branches anywhere.
    // Once a script runs out, conditions are false, so every loop ends.
    std::uint32_t seed = 12345;
    const auto random = [&](std::uint32_t n) {
        seed = seed * 1664525u + 1013904223u;
        return (seed >> 8) % n;
    };
    int irreducible = 0;
    for (int round = 0; round < 500; ++round) {
        const auto n = 4 + random(10);
        std::vector<std::uint32_t> words;
        for (std::uint32_t i = 0; i + 1 < n; ++i) {
            const std::uint32_t kind = random(20);
            const auto at = static_cast<int>(i);
            if (kind < 8) {
                words.push_back(sopp(nop));
            } else if (kind == 8) {
                words.push_back(sopp(endpgm));
            } else if (kind < 11) {
                words.push_back(br(branch, at, at + 1 + static_cast<int>(random(n - i - 1))));
            } else {
                words.push_back(br(scc0 + random(6), at, static_cast<int>(random(n))));
            }
        }
        words.push_back(sopp(endpgm));
        const Cfg cfg = build_cfg(decode(words, Until::code_end));
        irreducible += analyze(cfg).reducible ? 0 : 1;
        std::vector<Script> scripts(3);
        for (Script& script : scripts) {
            for (std::uint32_t b = 0; b < cfg.blocks.size(); ++b) {
                for (std::uint32_t k = random(4); k > 0; --k) {
                    script[b].push_back(random(2) == 0);
                }
            }
        }
        SCOPED_TRACE(round);
        check(words, scripts);
    }
    EXPECT_GT(irreducible, 20);
}

}  // namespace
