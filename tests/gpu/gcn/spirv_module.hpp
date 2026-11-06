// Walks the instructions of a SPIR-V module, for the tests that look at what the translator emits.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <spirv/unified1/spirv.hpp11>

namespace bb::test::gcn {

// The instructions of a module after its header, each with its opcode word first.
inline std::vector<std::span<const std::uint32_t>> instructions(
    const std::vector<std::uint32_t>& spirv) {
    std::vector<std::span<const std::uint32_t>> out;
    for (std::size_t i = 5; i < spirv.size() && (spirv[i] >> 16) != 0; i += spirv[i] >> 16) {
        out.push_back(std::span(spirv).subspan(i, std::min<std::size_t>(spirv[i] >> 16,
                                                                         spirv.size() - i)));
    }
    return out;
}

inline bool is(std::span<const std::uint32_t> inst, spv::Op op) {
    return (inst[0] & 0xffff) == static_cast<std::uint32_t>(op);
}

}  // namespace bb::test::gcn
