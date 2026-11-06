// Assembles GCN text with LLVM's llvm-mc, for the decoder's round-trip tests.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace bb::test::gcn {

// Whether CMake found llvm-mc. Tests that assemble skip without it.
bool have_llvm_mc();

struct Assembled {
    std::string text;                  // the instruction as llvm-mc prints it back
    std::vector<std::uint32_t> words;  // its encoding
};

// Each line assembled by `llvm-mc -triple=amdgcn -mcpu=bonaire -show-encoding`.
// Throws std::runtime_error with llvm-mc's output when it rejects a line.
std::vector<Assembled> assemble(const std::vector<std::string>& lines);

// The words of every instruction in the source file, in order. Comments and blank lines give
// none. Throws std::runtime_error with llvm-mc's output when it rejects the source, or when a
// branch names a label, whose offset -show-encoding leaves unresolved.
std::vector<std::vector<std::uint32_t>> assemble_file(const std::filesystem::path& source);

}  // namespace bb::test::gcn
