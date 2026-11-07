// Where the buffer descriptors of a program come from (docs/design/graphics.md, "Resources and
// memory"): a dataflow pass over the SGPRs.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "gpu/gcn/cfg.hpp"
#include "gpu/gcn/gcn.hpp"

namespace bb::gcn {

// The source of a V#: `path[0]` is the user-data slot of its first dword when `path` has one
// entry. With more, the descriptor is loaded from memory: the dwords at slot `path[0]` and the
// next one are a pointer, `path[1]` is the dword offset of the first descriptor dword in what it
// points to, and each further entry loads again through the two dwords at the previous one.
struct ResourceSource {
    std::vector<std::uint32_t> path;

    bool operator==(const ResourceSource&) const = default;
};

// The sources of the buffer descriptors that the program's buffer instructions use (MUBUF and
// s_buffer_load_*), each once, in the order of first use. A descriptor whose source can't be
// followed (computed, or a different one on two paths to the use) is left out. SGPRs 0 to
// `user_sgprs` - 1 start out as user-data slots.
std::vector<ResourceSource> find_buffer_sources(std::span<const Instruction> program,
                                                const Cfg& cfg, unsigned user_sgprs);

// The register operand that holds the descriptor of a buffer instruction, or nullptr.
const Operand* buffer_descriptor(const Instruction& inst);

}  // namespace bb::gcn
