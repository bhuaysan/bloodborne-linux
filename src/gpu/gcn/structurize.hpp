// Structured SPIR-V control flow from a GCN program's control flow graph (docs/design/graphics.md,
// "Control flow").
#pragma once

#include <cstdint>
#include <functional>

#include "gpu/gcn/cfg.hpp"
#include "gpu/gcn/spirv_builder.hpp"

namespace bb::gcn {

// What the structurizer asks of the translator, once per reachable block.
struct BlockEmitter {
    // Emits the block's instructions into the current SPIR-V block.
    std::function<void(std::uint32_t block)> body;
    // For a conditional exit: emits the branch condition after the body and returns its bool id.
    std::function<SpirvBuilder::Id(std::uint32_t block)> condition;
};

// Emits the graph as structured control flow into the current function. A block of the caller
// (its prologue) must be open; the structurizer ends it with a branch and returns with no block
// open. Acyclic regions and natural loops become selections and loops; a branch out of several
// constructs goes through a function variable. An irreducible graph becomes a loop around a
// switch on the index of the next block.
void structurize(const Cfg& cfg, SpirvBuilder& builder, const BlockEmitter& emitter);

}  // namespace bb::gcn
