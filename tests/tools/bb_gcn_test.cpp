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

namespace {

struct Result {
    int exit_code;  // -1 when the process didn't exit normally
    std::string output;
};

// Runs bb-gcn with `args` through /bin/sh and captures its standard output, and with
// `with_stderr` its standard error too.
Result bb_gcn(const std::string& args, bool with_stderr = false) {
    const std::string command =
        std::string(BB_GCN_PATH) + " " + args + (with_stderr ? " 2>&1" : " 2>/dev/null");
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        return {-1, "popen failed"};
    }
    std::string output;
    char buffer[4096];
    while (std::size_t n = std::fread(buffer, 1, sizeof buffer, pipe)) {
        output.append(buffer, n);
    }
    int status = pclose(pipe);
    return {WIFEXITED(status) ? WEXITSTATUS(status) : -1, output};
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

    std::filesystem::path file_;
};

TEST_F(BbGcn, DisassemblesUpToTheFinalEndpgm) {
    // s_mov_b32 s0, 0x12345678; s_endpgm; then a word that isn't an instruction and 3 bytes.
    write({0xbe8003ff, 0x12345678, 0xbf810000, 0xbf9b0000}, "end");
    const auto [status, output] = bb_gcn("disasm " + file_.string());
    EXPECT_EQ(status, 0);
    EXPECT_EQ(output,
              "s_mov_b32 s0, 0x12345678                         // 000000: be8003ff 12345678\n"
              "s_endpgm                                         // 000008: bf810000\n");
}

TEST_F(BbGcn, AllDecodesEveryWholeDword) {
    // s_nop 0, s_endpgm, s_nop 1, then 3 bytes that aren't a whole dword.
    write({0xbf800000, 0xbf810000, 0xbf800001}, "end");
    const auto [status, output] = bb_gcn("disasm --all " + file_.string());
    EXPECT_EQ(status, 0);
    EXPECT_EQ(output,
              "s_nop 0                                          // 000000: bf800000\n"
              "s_endpgm                                         // 000004: bf810000\n"
              "s_nop 1                                          // 000008: bf800001\n");

    // Opcode 27 isn't a SOPP instruction: the error names its offset and word.
    write({0xbf810000, 0xbf9b0000});
    const auto [error_status, error] = bb_gcn("disasm --all " + file_.string(), true);
    EXPECT_EQ(error_status, 1);
    EXPECT_EQ(error, "bb-gcn: unknown SOPP opcode 27 at offset 0x4 (word 0xbf9b0000)\n");
}

TEST_F(BbGcn, RejectsBadArguments) {
    write({0xbf810000});
    EXPECT_EQ(bb_gcn("").exit_code, 2);
    EXPECT_EQ(bb_gcn("disasm").exit_code, 2);
    EXPECT_EQ(bb_gcn("disasm --size 4 " + file_.string()).exit_code, 2);
    EXPECT_EQ(bb_gcn("disasm " + file_.string() + " " + file_.string()).exit_code, 2);
    EXPECT_EQ(bb_gcn("translate " + file_.string()).exit_code, 2);
    EXPECT_EQ(bb_gcn("--help").exit_code, 0);
    std::filesystem::remove(file_);
    EXPECT_EQ(bb_gcn("disasm " + file_.string()).exit_code, 1);  // no such file
}

}  // namespace
