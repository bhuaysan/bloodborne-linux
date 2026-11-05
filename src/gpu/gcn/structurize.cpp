// Structured SPIR-V control flow from a reducible graph (SPIR-V specification, "Structured Control
// Flow"; docs/design/graphics.md, "Control flow").
//
// The nesting follows N. Ramsey, "Beyond Relooper" (ICFP 2022). Every block is emitted once:
// where its only forward predecessor branches to it, or, when it has several (a merge node),
// right after a scope opened in the code of its immediate dominator. Merge nodes are ordered by
// reverse postorder (CfgAnalysis).
//
// In SPIR-V both kinds of construct are loops. A scope is a loop that runs once, so a branch to
// its merge node is a break from anywhere inside it. A natural loop is a loop whose continue block
// branches back to the header, so a back edge is a continue; nothing breaks it. A conditional
// block is a selection whose merge block is unreachable: both arms leave it by a break, a
// continue or a return. Breaks and continues may only go to the innermost loop, so a branch to an
// outer construct stores the construct's depth in a function variable and breaks. The merge
// block of each construct it leaves passes it on, until the target clears the variable.

#include <algorithm>
#include <stdexcept>

#include "gpu/gcn/structurize.hpp"

namespace bb::gcn {

namespace {

using Id = SpirvBuilder::Id;
using Op = spv::Op;

constexpr std::uint32_t no_control = spirv_word(spv::SelectionControlMask::MaskNone);

class Structurizer {
public:
    Structurizer(const Cfg& cfg, SpirvBuilder& builder, const BlockEmitter& emitter)
        : cfg_(cfg), b_(builder), emitter_(emitter) {}

    void run() {
        analyze();
        const Id first = b_.id();
        b_.op_void(Op::OpBranch, {first});
        tree(0, first);
    }

private:
    struct Construct {
        bool loop;           // a natural loop (branches to it continue), else a scope (they break)
        std::uint32_t node;  // the loop header, or the scope's merge node
        Id merge, cont;
        bool crossed = false;  // a branch to an outer construct leaves through its merge block
    };

    void analyze() {
        a_ = gcn::analyze(cfg_);
        for (std::uint32_t node : a_.rpo) {
            for (std::uint32_t s : successors(cfg_.blocks[node])) {
                if (a_.order[s] <= a_.order[node] && !a_.dominates(s, node)) {
                    throw GcnError("irreducible control flow", cfg_.blocks[node].offset, 0);
                }
            }
        }
        merge_children_.assign(cfg_.blocks.size(), {});
        for (std::uint32_t node : a_.rpo) {  // in reverse postorder, so the children stay sorted
            if (a_.merge_node[node]) {
                merge_children_[a_.idom[node]].push_back(node);
            }
        }
    }

    // Emits `node` and the blocks it dominates, starting with the block `label`.
    void tree(std::uint32_t node, Id label) {
        if (!a_.loop_header[node]) {
            within(node, merge_children_[node], label);
            return;
        }
        const Id body =
            open({.loop = true, .node = node, .merge = b_.id(), .cont = b_.id()}, label);
        within(node, merge_children_[node], body);
        const Construct loop = close(label);
        b_.begin_block(loop.merge);
        if (loop.crossed) {
            pass_on(b_.op(Op::OpLoad, b_.type_int(32, false), {pending()}));
        } else {
            b_.op_void(Op::OpUnreachable);
        }
    }

    // Emits `node` inside the scopes of the merge nodes it dominates (sorted by reverse
    // postorder, so the last one is the outermost), and each merge node after its scope.
    void within(std::uint32_t node, std::span<const std::uint32_t> merges, Id label) {
        if (merges.empty()) {
            emit(node, label);
            return;
        }
        const std::uint32_t outer = merges.back();
        const Id body = open({.loop = false, .node = outer, .merge = b_.id(), .cont = b_.id()},
                             label);
        within(node, merges.first(merges.size() - 1), body);
        const Construct scope = close(label);
        if (!scope.crossed) {
            tree(outer, scope.merge);
            return;
        }
        // Arriving with the variable set means passing through to an outer construct.
        b_.begin_block(scope.merge);
        const Id u32 = b_.type_int(32, false);
        const Id target = b_.op(Op::OpLoad, u32, {pending()});
        const Id none = b_.op(Op::OpIEqual, b_.type_bool(), {target, b_.constant_u32(0)});
        const Id here = b_.id();
        const Id on = b_.id();
        b_.op_void(Op::OpSelectionMerge, {here, no_control});
        b_.op_void(Op::OpBranchConditional, {none, here, on});
        b_.begin_block(on);
        pass_on(target);
        tree(outer, here);
    }

    // Emits the header of a construct in the block `label` and returns the label of its body.
    Id open(const Construct& construct, Id label) {
        b_.begin_block(label);
        const Id body = b_.id();
        b_.op_void(Op::OpLoopMerge,
                   {construct.merge, construct.cont, spirv_word(spv::LoopControlMask::MaskNone)});
        b_.op_void(Op::OpBranch, {body});
        stack_.push_back(construct);
        return body;
    }

    // Ends the innermost construct with its continue block, which branches back to `header`.
    Construct close(Id header) {
        const Construct construct = stack_.back();
        stack_.pop_back();
        b_.begin_block(construct.cont);
        b_.op_void(Op::OpBranch, {header});
        return construct;
    }

    // Emits the block itself, starting with `label`.
    void emit(std::uint32_t node, Id label) {
        const Block& block = cfg_.blocks[node];
        b_.begin_block(label);
        emitter_.body(node);
        switch (block.exit) {
        case Exit::end:
            b_.op_void(Op::OpReturn);
            break;
        case Exit::next:
            branch(node, block.next);
            break;
        case Exit::jump:
            branch(node, block.taken);
            break;
        case Exit::conditional: {
            const Id condition = emitter_.condition(node);
            const Id taken = b_.id();
            const Id next = b_.id();
            const Id merge = b_.id();
            b_.op_void(Op::OpSelectionMerge, {merge, no_control});
            b_.op_void(Op::OpBranchConditional, {condition, taken, next});
            b_.begin_block(taken);
            branch(node, block.taken);
            b_.begin_block(next);
            branch(node, block.next);
            b_.begin_block(merge);
            b_.op_void(Op::OpUnreachable);
            break;
        }
        }
    }

    // Ends the current block with the edge from `from` to `to`.
    void branch(std::uint32_t from, std::uint32_t to) {
        const bool back = a_.order[to] <= a_.order[from];
        if (!back && !a_.merge_node[to]) {
            const Id label = b_.id();
            b_.op_void(Op::OpBranch, {label});
            tree(to, label);
            return;
        }
        const auto it = std::ranges::find_if(stack_, [&](const Construct& c) {
            return c.loop == back && c.node == to;
        });
        if (it == stack_.end()) {
            throw std::logic_error("structurizer: branch target is not an enclosing construct");
        }
        const auto depth = static_cast<std::size_t>(it - stack_.begin());
        if (depth + 1 == stack_.size()) {
            b_.op_void(Op::OpBranch, {it->loop ? it->cont : it->merge});
            return;
        }
        const auto value = static_cast<std::uint32_t>(depth + 1);
        b_.op_void(Op::OpStore, {pending(), b_.constant_u32(value)});
        for (auto c = stack_.begin() + static_cast<std::ptrdiff_t>(depth) + 1; c != stack_.end();
             ++c) {
            c->crossed = true;
        }
        b_.op_void(Op::OpBranch, {stack_.back().merge});
    }

    // Ends the current block by passing a stored branch to the innermost construct (`target` is
    // the variable's value): taking it and clearing the variable when the construct is the
    // target, otherwise breaking out of it.
    void pass_on(Id target) {
        const Construct& c = stack_.back();
        const Id zero = b_.constant_u32(0);
        const Id here = b_.op(Op::OpIEqual, b_.type_bool(),
                              {target, b_.constant_u32(static_cast<std::uint32_t>(stack_.size()))});
        if (!c.loop) {
            const Id kept = b_.op(Op::OpSelect, b_.type_int(32, false), {here, zero, target});
            b_.op_void(Op::OpStore, {pending(), kept});
            b_.op_void(Op::OpBranch, {c.merge});
            return;
        }
        const Id take = b_.id();
        const Id merge = b_.id();
        b_.op_void(Op::OpSelectionMerge, {merge, no_control});
        b_.op_void(Op::OpBranchConditional, {here, take, c.merge});
        b_.begin_block(take);
        b_.op_void(Op::OpStore, {pending(), zero});
        b_.op_void(Op::OpBranch, {c.cont});
        b_.begin_block(merge);
        b_.op_void(Op::OpUnreachable);
    }

    // The function variable that holds a stored branch: the depth of its target plus 1, or 0.
    Id pending() {
        if (pending_ == 0) {
            const Id u32 = b_.type_int(32, false);
            pending_ = b_.local_variable(b_.type_pointer(spv::StorageClass::Function, u32),
                                         b_.constant_u32(0));
        }
        return pending_;
    }

    const Cfg& cfg_;
    SpirvBuilder& b_;
    const BlockEmitter& emitter_;
    CfgAnalysis a_;
    std::vector<std::vector<std::uint32_t>> merge_children_;  // in reverse postorder
    std::vector<Construct> stack_;                            // the open constructs, innermost last
    Id pending_ = 0;
};

}  // namespace

void structurize(const Cfg& cfg, SpirvBuilder& builder, const BlockEmitter& emitter) {
    Structurizer(cfg, builder, emitter).run();
}

}  // namespace bb::gcn
