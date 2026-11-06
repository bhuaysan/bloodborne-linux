// The fetch subroutine of the vertex stage, inlined at its call (docs/design/graphics.md, "Vertex
// fetch" and "Fetch subroutine"). Internal to the translator.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "gpu/gcn/gcn.hpp"

namespace bb::gcn::detail {

// Where the fetch subroutine went into the program.
struct FetchSite {
    static constexpr std::uint32_t none = UINT32_MAX;

    std::uint32_t call = none;  // index of the s_swappc_b64, or none
    // The byte offset in the fetch code of each inlined instruction: program[call + 1 + i] is the
    // instruction at offsets[i].
    std::vector<std::uint32_t> offsets;

    // Whether program[index] came from the fetch subroutine.
    bool inlined(std::uint32_t index) const {
        return call != none && index > call && index - call <= offsets.size();
    }
};

// Decodes `fetch` up to its s_setpc_b64 and puts its other instructions after the program's
// s_swappc_b64, which stays: the translator writes the return address into its destination. The
// inlined instructions take the call's byte offset, so the offsets stay in order for the branch
// targets. Nothing happens to a program without a call. Throws GcnError at the call's offset for
// a call through another pair than s[0:1], a second call, a missing fetch subroutine, and a fetch
// subroutine that can't be inlined: one that branches, ends the program, writes its return
// address or returns through another pair.
FetchSite inline_fetch(std::vector<Instruction>& program, std::span<const std::uint32_t> fetch);

}  // namespace bb::gcn::detail
