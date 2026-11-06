// Control flow graph of a GCN program, from its scalar branches (Sea Islands ISA manual, "SOPP
// Instructions"; docs/design/graphics.md, "Execz skips and live-out scalars"), and its analysis.
// Dominators are computed with the iterative algorithm of K. D. Cooper, T. J. Harvey and
// K. Kennedy, "A Simple, Fast Dominance Algorithm" (2001).

#include "gpu/gcn/cfg.hpp"

#include <algorithm>
#include <format>
#include <iterator>
#include <string_view>
#include <utility>

namespace bb::gcn {

namespace {

// SOPP opcodes of the branches.
constexpr std::uint16_t op_branch = 2;
constexpr std::uint16_t op_cbranch_first = 4;  // s_cbranch_scc0, then scc1, vccz, vccnz, execz
constexpr std::uint16_t op_cbranch_execz = 8;
constexpr std::uint16_t op_cbranch_last = 9;  // s_cbranch_execnz

void check_supported(const Instruction& inst) {
    constexpr std::string_view unsupported[] = {"s_setpc_b64", "s_cbranch_join",
                                                "s_cbranch_i_fork", "s_cbranch_g_fork"};
    // The other SOPP branches are the s_cbranch_cdbg* debug branches.
    const bool debug_branch = inst.format == Format::sopp && branch_target(inst) &&
                              inst.opcode != op_branch &&
                              (inst.opcode < op_cbranch_first || inst.opcode > op_cbranch_last);
    if (debug_branch || std::ranges::find(unsupported, inst.name) != std::end(unsupported)) {
        throw GcnError(std::format("unsupported branch {}", inst.name), inst.offset, 0);
    }
}

// Whether the instruction may write an SGPR, M0, VCC, EXEC or SCC. The scalar ALU and memory
// formats all do (SCC or their destination); other formats name their scalar destinations.
bool writes_scalars(const Instruction& inst) {
    switch (inst.format) {
    case Format::sop2:
    case Format::sopk:
    case Format::sop1:
    case Format::sopc:
    case Format::smrd:
        return true;
    default:
        return std::ranges::any_of(std::span(inst.dst).first(inst.num_dst), [](const Operand& o) {
            return o.kind == OperandKind::scalar;
        });
    }
}

// Whether the forward branch that ends block `from` skips a region (the blocks between it and its
// target) that is entered only from `from`, is left only to the target, and writes scalars.
bool skips_scalar_writes(const Cfg& cfg, std::span<const Instruction> program,
                         std::uint32_t from) {
    const std::uint32_t target = cfg.blocks[from].taken;
    const auto inside = [&](std::uint32_t b) { return b > from && b < target; };
    bool writes = false;
    for (std::uint32_t b = 0; b < cfg.blocks.size(); ++b) {
        const Block& block = cfg.blocks[b];
        for (std::uint32_t s : successors(block)) {
            const bool allowed = inside(b) ? inside(s) || s == target
                                           : !inside(s) || (b == from && s == from + 1);
            if (!allowed) {
                return false;
            }
        }
        if (inside(b)) {
            if (block.exit == Exit::end) {
                return false;
            }
            writes = writes ||
                     std::ranges::any_of(program.subspan(block.first, block.count), writes_scalars);
        }
    }
    return writes;
}

}  // namespace

std::vector<std::uint32_t> successors(const Block& block) {
    switch (block.exit) {
    case Exit::next:
        return {block.next};
    case Exit::jump:
        return {block.taken};
    case Exit::conditional:
        return {block.taken, block.next};
    case Exit::end:
        break;
    }
    return {};
}

Cfg build_cfg(std::span<const Instruction> program) {
    if (program.empty()) {
        throw GcnError("empty program", 0, 0);
    }
    // The index of the instruction that a branch lands on.
    const auto target_index = [&](const Instruction& branch) {
        const std::uint32_t target = *branch_target(branch);
        const auto it = std::ranges::lower_bound(program, target, {}, &Instruction::offset);
        if (it == program.end() || it->offset != target) {
            throw GcnError(std::format("branch target {:#x} is not an instruction", target),
                           branch.offset, 0);
        }
        return static_cast<std::uint32_t>(it - program.begin());
    };

    std::vector<bool> starts(program.size() + 1, false);  // a block starts at this instruction
    starts[0] = true;
    for (std::size_t i = 0; i < program.size(); ++i) {
        check_supported(program[i]);
        if (branch_target(program[i])) {
            starts[target_index(program[i])] = true;
        }
        starts[i + 1] = starts[i + 1] || branch_target(program[i]) || is_endpgm(program[i]);
    }
    Cfg cfg;
    std::vector<std::uint32_t> block_of(program.size());
    for (std::uint32_t i = 0; i < program.size(); ++i) {
        if (starts[i]) {
            cfg.blocks.push_back({.offset = program[i].offset, .first = i});
        }
        block_of[i] = static_cast<std::uint32_t>(cfg.blocks.size() - 1);
        ++cfg.blocks.back().count;
    }

    for (std::uint32_t b = 0; b < cfg.blocks.size(); ++b) {
        Block& block = cfg.blocks[b];
        const Instruction& last = program[block.first + block.count - 1];
        block.next = b + 1;
        if (is_endpgm(last)) {
            block.exit = Exit::end;
        } else if (branch_target(last)) {
            block.taken = block_of[target_index(last)];
            block.exit = last.opcode == op_branch      ? Exit::jump
                         : block.taken == block.next ? Exit::next
                                                     : Exit::conditional;
        }
        const bool falls = block.exit == Exit::next || block.exit == Exit::conditional;
        if (falls && block.next == cfg.blocks.size()) {
            throw GcnError("control runs off the end of the program", last.offset, 0);
        }
    }
    // The order doesn't matter: a nested skip's taken edge stays inside the outer region or on
    // its target either way, and two overlapping skips each give the other a second entry or exit.
    for (std::uint32_t b = 0; b < cfg.blocks.size(); ++b) {
        Block& block = cfg.blocks[b];
        const Instruction& last = program[block.first + block.count - 1];
        if (block.exit == Exit::conditional && last.opcode == op_cbranch_execz &&
            block.taken > b && skips_scalar_writes(cfg, program, b)) {
            block.exit = Exit::next;
        }
    }
    return cfg;
}

bool CfgAnalysis::dominates(std::uint32_t a, std::uint32_t b) const {
    for (;; b = idom[b]) {
        if (b == a) {
            return true;
        }
        if (b == 0) {  // the entry
            return false;
        }
    }
}

CfgAnalysis analyze(const Cfg& cfg) {
    const auto n = static_cast<std::uint32_t>(cfg.blocks.size());
    CfgAnalysis a;
    // Postorder by an iterative depth-first search from the entry.
    std::vector<std::uint32_t> post;
    std::vector<bool> seen(n, false);
    std::vector<std::pair<std::uint32_t, std::size_t>> stack = {{0, 0}};  // block, next successor
    seen[0] = true;
    while (!stack.empty()) {
        const std::uint32_t block = stack.back().first;
        const auto succ = successors(cfg.blocks[block]);
        if (stack.back().second == succ.size()) {
            post.push_back(block);
            stack.pop_back();
        } else if (const std::uint32_t s = succ[stack.back().second++]; !seen[s]) {
            seen[s] = true;
            stack.push_back({s, 0});
        }
    }
    a.rpo.assign(post.rbegin(), post.rend());
    a.order.assign(n, CfgAnalysis::none);
    for (std::uint32_t i = 0; i < a.rpo.size(); ++i) {
        a.order[a.rpo[i]] = i;
    }
    std::vector<std::vector<std::uint32_t>> preds(n);
    for (std::uint32_t block : a.rpo) {
        for (std::uint32_t s : successors(cfg.blocks[block])) {
            preds[s].push_back(block);
        }
    }

    // The nearest common dominator of two blocks whose dominators are known so far.
    const auto intersect = [&](std::uint32_t x, std::uint32_t y) {
        while (x != y) {
            while (a.order[x] > a.order[y]) {
                x = a.idom[x];
            }
            while (a.order[y] > a.order[x]) {
                y = a.idom[y];
            }
        }
        return x;
    };
    a.idom.assign(n, CfgAnalysis::none);
    a.idom[0] = 0;
    for (bool changed = true; changed;) {
        changed = false;
        for (std::uint32_t block : std::span(a.rpo).subspan(1)) {
            std::uint32_t dom = CfgAnalysis::none;
            for (std::uint32_t p : preds[block]) {
                if (a.idom[p] != CfgAnalysis::none) {
                    dom = dom == CfgAnalysis::none ? p : intersect(p, dom);
                }
            }
            changed = changed || a.idom[block] != dom;
            a.idom[block] = dom;
        }
    }

    a.loop_header.assign(n, false);
    a.merge_node.assign(n, false);
    std::vector<std::uint32_t> forward_preds(n, 0);
    for (std::uint32_t block : a.rpo) {
        for (std::uint32_t s : successors(cfg.blocks[block])) {
            if (a.order[s] > a.order[block]) {
                a.merge_node[s] = ++forward_preds[s] > 1;
            } else if (a.dominates(s, block)) {
                a.loop_header[s] = true;
            } else {
                a.reducible = false;
            }
        }
    }
    return a;
}

}  // namespace bb::gcn
