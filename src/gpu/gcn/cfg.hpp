// Control flow graph of a GCN program and its analysis (docs/design/graphics.md, "Control
// flow").
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "gpu/gcn/gcn.hpp"

namespace bb::gcn {

// How a block ends. Its last instruction is the branch or s_endpgm, if it has one; the
// translator emits nothing for that instruction, its effect is the exit.
enum class Exit : std::uint8_t {
    next,         // falls through to `next`, also after a forward execz skip that became one
    jump,         // s_branch to `taken`
    conditional,  // s_cbranch_*: to `taken` when the condition holds, otherwise to `next`
    end,          // s_endpgm
};

struct Block {
    std::uint32_t offset = 0;  // byte offset of the first instruction, for errors
    std::uint32_t first = 0;   // index of the first instruction
    std::uint32_t count = 0;   // number of instructions, the branch included
    Exit exit = Exit::next;
    std::uint32_t taken = 0;   // block index (jump, conditional)
    std::uint32_t next = 0;    // block index (next, conditional)
};

// The blocks control can go to next: `taken` first, then `next`.
std::vector<std::uint32_t> successors(const Block& block);

// Blocks in program order; block 0 is the entry.
struct Cfg {
    std::vector<Block> blocks;
};

// Splits a decoded program into blocks. A forward s_cbranch_execz that skips a single-entry region
// whose only exit is the target, and in which scalars are written, becomes a fall-through. Throws
// GcnError for a branch that doesn't land on an instruction, control running off the end, and the
// unsupported branches (fork and join, s_setpc_b64, the debug branches).
Cfg build_cfg(std::span<const Instruction> program);

// Orders, dominators and loops of the blocks reachable from the entry.
struct CfgAnalysis {
    static constexpr std::uint32_t none = UINT32_MAX;

    std::vector<std::uint32_t> rpo;    // the reachable blocks in reverse postorder
    std::vector<std::uint32_t> order;  // each block's position in `rpo`, or `none`
    std::vector<std::uint32_t> idom;   // immediate dominator (the entry's is itself), or `none`
    // An edge to a block at the same or an earlier position in `rpo` is a back edge. Its target
    // is a loop header when it dominates the edge's source; otherwise the graph is irreducible.
    std::vector<bool> loop_header;
    std::vector<bool> merge_node;  // more than one forward edge comes in
    bool reducible = true;

    // Whether reachable block `a` dominates reachable block `b`.
    bool dominates(std::uint32_t a, std::uint32_t b) const;
};

CfgAnalysis analyze(const Cfg& cfg);

}  // namespace bb::gcn
