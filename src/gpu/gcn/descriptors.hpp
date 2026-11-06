// Where the buffer descriptors of a program come from (docs/design/graphics.md, "Resources and
// memory"): a dataflow pass over the SGPRs.
#pragma once

#include <array>
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

// The run time's check of a V# it resolved from a source in `ShaderInterface::buffers`, before the
// shader runs. Throws GcnError for a V# with `swizzle_enable` (dword 1, bit 31),
// `add_tid_enable` (dword 3, bit 23) or `heap` (dword 3, bit 26): they change the address or the
// range check (Sea Islands ISA 8.1.5.2, Table 8.5), which the translator does not implement yet
// (scratch, #247), and it would otherwise ignore them silently. The error's offset is the byte
// offset of the dword in the V#, its word that dword. A V# that the shader computes is out of
// reach and not checked. `s_buffer_load_*` ignores these bits (7.2.2), but
// `ShaderInterface::buffers` doesn't say which instruction uses a source, so the check is applied
// to every source.
void check_buffer_descriptor(const std::array<std::uint32_t, 4>& vsharp);

// The register operand that holds the descriptor of a buffer instruction, or nullptr.
const Operand* buffer_descriptor(const Instruction& inst);

}  // namespace bb::gcn
