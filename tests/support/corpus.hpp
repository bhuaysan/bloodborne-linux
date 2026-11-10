// Reading the shader corpus that tools/shaderbnd extracted from the game (docs/formats/
// shader-binary.md), for the corpus tests.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace bb::test::gcn {

// The footer's program type (bits 2 to 5 of the u32 at +8 of the footer block).
enum class ProgramType : unsigned { ps, vs, es, ls, cs, gs, copy_vs, hs, ds };

// The code of one program file: the file holds the code, the footer data and the footer block,
// which says how long the code is and what kind of program it is.
struct Program {
    std::vector<std::uint32_t> code;
    unsigned type = 0;  // a ProgramType, unchecked
    // The first 16 dwords of the register block in the entry header (from 0x3c), for the first
    // program of an entry; `has_block` is false for any other program.
    std::array<std::uint32_t, 16> block{};
    bool has_block = false;
};

// Reads one program file. An empty `code` with `why` set when the file isn't a program.
Program read_program(const std::filesystem::path& file, std::string& why);

// The program files of every extracted archive under `root` (analysis/shaders), in a fixed order.
std::vector<std::filesystem::path> program_files(const std::filesystem::path& root);

// The type's name for reports: "ps", "vs", "es", "ls", "cs", "gs", "copy-vs", "hs", "ds".
const char* program_type_name(unsigned type);

}  // namespace bb::test::gcn
