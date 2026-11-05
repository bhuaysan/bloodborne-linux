// Assembles GCN text with LLVM's llvm-mc, for the decoder's round-trip tests.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bb::test::gcn {

// Whether CMake found llvm-mc. Tests that assemble skip without it.
bool have_llvm_mc();

// The words of each line, assembled by `llvm-mc -triple=amdgcn -mcpu=bonaire -show-encoding`.
// Throws std::runtime_error with llvm-mc's output when it rejects a line.
std::vector<std::vector<std::uint32_t>> assemble(const std::vector<std::string>& lines);

}  // namespace bb::test::gcn
