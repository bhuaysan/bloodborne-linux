// The test program format: every program in tests/gpu/programs/ has its source and its words, and
// the words are what llvm-mc makes of the source.

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "gpu/gcn/llvm_mc.hpp"
#include "gpu/gcn/programs.hpp"

namespace {

using namespace bb::test::gcn;
using Instructions = std::vector<std::vector<std::uint32_t>>;

TEST(GcnPrograms, ParseHex) {
    EXPECT_EQ(parse_hex(""), Instructions{});
    EXPECT_EQ(parse_hex("be8003ff 12345678\nbf810000\n"),
              (Instructions{{0xbe8003ff, 0x12345678}, {0xbf810000}}));
}

TEST(GcnPrograms, ParseHexRejectsOtherText) {
    for (const char* text : {"bf810000", "\n", "bf81000\n", "BF810000\n", "bf810000 \n",
                             "bf810000  12345678\n", "bf810000\tbf810000\n", "0xbf81000\n",
                             "; s_endpgm\n", "00000001 00000002 00000003\n"}) {
        SCOPED_TRACE(text);
        EXPECT_THROW(parse_hex(text), std::runtime_error);
    }
}

TEST(GcnPrograms, EveryProgramHasSourceAndWords) {
    const auto names = program_names();
    ASSERT_FALSE(names.empty());
    for (const auto& name : names) {
        SCOPED_TRACE(name);
        EXPECT_TRUE(std::filesystem::is_regular_file(programs_dir() / (name + ".s")));
        EXPECT_FALSE(program_instructions(name).empty());
    }
}

TEST(GcnPrograms, ExampleWords) {
    EXPECT_EQ(program_words("smov_literal"),
              (std::vector<std::uint32_t>{0xbe8003ff, 0x12345678, 0xbe8103c1, 0xbf810000}));
}

TEST(GcnPrograms, LabelsAreRejected) {
    if (!have_llvm_mc()) {
        GTEST_SKIP() << "llvm-mc not found";
    }
    EXPECT_THROW(assemble({"s_branch next", "next: s_endpgm"}), std::runtime_error);
}

TEST(GcnPrograms, WordsMatchSource) {
    if (!have_llvm_mc()) {
        GTEST_SKIP() << "llvm-mc not found";
    }
    for (const auto& name : program_names()) {
        SCOPED_TRACE(name);
        EXPECT_EQ(assemble_file(programs_dir() / (name + ".s")), program_instructions(name));
    }
}

}  // namespace
