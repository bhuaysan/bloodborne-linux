// Our GCN test programs in tests/gpu/programs/: <name>.s, the assembly source, and <name>.hex,
// its assembled words (docs/design/graphics.md, "Offline test strategy").
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace bb::test::gcn {

// tests/gpu/programs/ in the source tree.
std::filesystem::path programs_dir();

// The names of every program, from its .s or .hex file, sorted.
std::vector<std::string> program_names();

// The words of .hex text: one instruction per line, 1 or 2 words of 8 lower-case hex digits, the
// words of a line separated by one space. Throws std::runtime_error naming the line otherwise.
std::vector<std::vector<std::uint32_t>> parse_hex(std::string_view text);

// The COMPUTE_PGM_RSRC2 a program is translated with. The number of user SGPRs: the memory
// programs (mem_*) have 13, in s0 to s12, and the others have 5 (the masks go above them). Every
// program has 12 KB of LDS.
std::uint32_t program_pgm_rsrc2(std::string_view name);

// The instructions of programs_dir()/<name>.hex, and all their words in order.
std::vector<std::vector<std::uint32_t>> program_instructions(std::string_view name);
std::vector<std::uint32_t> program_words(std::string_view name);

}  // namespace bb::test::gcn
