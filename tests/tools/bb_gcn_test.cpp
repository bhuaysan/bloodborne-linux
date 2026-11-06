// Runs the bb-gcn binary on small programs written to a temporary file.

#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "support/process.hpp"

namespace {

using bb::test::quote;
using bb::test::Result;

// Runs bb-gcn with `args` (shell words) through /bin/sh and captures its standard output, and
// with `with_stderr` its standard error too.
Result bb_gcn(const std::string& args, bool with_stderr = false) {
    return bb::test::run(
        quote(BB_GCN_PATH) + " " + args + (with_stderr ? " 2>&1" : " 2>/dev/null"));
}

class BbGcn : public ::testing::Test {
protected:
    void SetUp() override {
        file_ = std::filesystem::temp_directory_path() /
                ("bb-gcn-test-" + std::to_string(getpid()) + ".bin");
    }
    void TearDown() override { std::filesystem::remove(file_); }

    // `words` as little-endian dwords, then the bytes of `tail`.
    void write(const std::vector<std::uint32_t>& words, const std::string& tail = "") {
        std::ofstream out(file_, std::ios::binary);
        for (std::uint32_t w : words) {
            for (unsigned i = 0; i < 4; ++i) {
                out.put(static_cast<char>(w >> (8 * i)));
            }
        }
        out << tail;
    }

    // The file as a shell word.
    std::string path() const { return quote(file_.string()); }

    std::filesystem::path file_;
};

TEST_F(BbGcn, DisassemblesUpToTheFinalEndpgm) {
    // s_mov_b32 s0, 0x12345678; s_endpgm; then a word that isn't an instruction and 3 bytes.
    write({0xbe8003ff, 0x12345678, 0xbf810000, 0xbf9b0000}, "end");
    const auto [status, output] = bb_gcn("disasm " + path());
    EXPECT_EQ(status, 0);
    EXPECT_EQ(output,
              "s_mov_b32 s0, 0x12345678                         // 000000: be8003ff 12345678\n"
              "s_endpgm                                         // 000008: bf810000\n");
}

TEST_F(BbGcn, ContinuesPastAnEndpgmThatABranchSkips) {
    // s_cbranch_scc0 over the first s_endpgm, so the program ends at the second one.
    write({0xbf840001, 0xbf810000, 0xbf810000, 0xbf800000});
    const auto [status, output] = bb_gcn("disasm " + path());
    EXPECT_EQ(status, 0);
    EXPECT_EQ(output,
              "s_cbranch_scc0 1                                 // 000000: bf840001\n"
              "s_endpgm                                         // 000004: bf810000\n"
              "s_endpgm                                         // 000008: bf810000\n");
}

TEST_F(BbGcn, ReportsAProgramWithoutEndpgm) {
    // s_nop 0 and 3 bytes: the program runs out of code at the whole dwords' end.
    write({0xbf800000}, "end");
    const auto [status, error] = bb_gcn("disasm " + path(), true);
    EXPECT_EQ(status, 1);
    EXPECT_EQ(error,
              "bb-gcn: no s_endpgm at or beyond the furthest branch target at offset 0x4 "
              "(word 0x00000000)\n");
}

TEST_F(BbGcn, AllDecodesEveryWholeDword) {
    // s_nop 0, s_endpgm, s_nop 1, then 3 bytes that aren't a whole dword.
    write({0xbf800000, 0xbf810000, 0xbf800001}, "end");
    const auto [status, output] = bb_gcn("disasm --all " + path());
    EXPECT_EQ(status, 0);
    EXPECT_EQ(output,
              "s_nop 0                                          // 000000: bf800000\n"
              "s_endpgm                                         // 000004: bf810000\n"
              "s_nop 1                                          // 000008: bf800001\n");

    // Opcode 27 isn't a SOPP instruction: the error names its offset and word.
    write({0xbf810000, 0xbf9b0000});
    const auto [error_status, error] = bb_gcn("disasm --all " + path(), true);
    EXPECT_EQ(error_status, 1);
    EXPECT_EQ(error, "bb-gcn: unknown SOPP opcode 27 at offset 0x4 (word 0xbf9b0000)\n");
}

TEST_F(BbGcn, RejectsBadArguments) {
    write({0xbf810000});
    EXPECT_EQ(bb_gcn("").status, 2);
    EXPECT_EQ(bb_gcn("disasm").status, 2);
    EXPECT_EQ(bb_gcn("disasm --size 4 " + path()).status, 2);
    EXPECT_EQ(bb_gcn("disasm " + path() + " " + path()).status, 2);
    EXPECT_EQ(bb_gcn("translate " + path()).status, 2);
    EXPECT_EQ(bb_gcn("--help").status, 0);
    std::filesystem::remove(file_);
    EXPECT_EQ(bb_gcn("disasm " + path()).status, 1);  // no such file
}

}  // namespace
