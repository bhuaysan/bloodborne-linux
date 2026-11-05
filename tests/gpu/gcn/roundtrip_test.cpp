// Round trips through LLVM's assembler: every opcode of the tables, written from the Sea Islands
// ISA manual, is assembled by llvm-mc and decoded again.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "gpu/gcn/encode.hpp"
#include "gpu/gcn/gcn.hpp"
#include "gpu/gcn/llvm_mc.hpp"

namespace {

using namespace bb::gcn;
using namespace bb::test::gcn;

// Every SOPP opcode, with 5 as the immediate where it has one.
const std::vector<std::string> sopp_lines = {
    "s_nop 5",           "s_endpgm",           "s_branch 5",
    "s_cbranch_scc0 5",  "s_cbranch_scc1 5",   "s_cbranch_vccz 5",
    "s_cbranch_vccnz 5", "s_cbranch_execz 5",  "s_cbranch_execnz 5",
    "s_barrier",         "s_setkill 5",        "s_waitcnt 5",
    "s_sethalt 5",       "s_sleep 5",          "s_setprio 5",
    "s_sendmsg 5",       "s_sendmsghalt 5",    "s_trap 5",
    "s_icache_inv",      "s_incperflevel 5",   "s_decperflevel 5",
    "s_ttracedata",      "s_cbranch_cdbgsys 5", "s_cbranch_cdbguser 5",
    "s_cbranch_cdbgsys_or_user 5", "s_cbranch_cdbgsys_and_user 5",
};

TEST(GcnRoundTrip, SoppTableHasNoOtherOpcodes) {
    unsigned known = 0;
    for (unsigned op = 0; op < 128; ++op) {
        try {
            decode(std::vector<std::uint32_t>{sopp(op)}, Until::code_end);
            ++known;
        } catch (const GcnError&) {
        }
    }
    EXPECT_EQ(known, sopp_lines.size());
}

TEST(GcnRoundTrip, Sopp) {
    if (!have_llvm_mc()) {
        GTEST_SKIP() << "llvm-mc not found";
    }
    const auto words = assemble(sopp_lines);
    for (std::size_t i = 0; i < sopp_lines.size(); ++i) {
        SCOPED_TRACE(sopp_lines[i]);
        const auto insts = decode(words[i], Until::code_end);
        ASSERT_EQ(insts.size(), 1u);
        const Instruction& inst = insts[0];
        const std::string_view line = sopp_lines[i];
        const auto space = line.find(' ');
        EXPECT_EQ(inst.format, Format::sopp);
        EXPECT_EQ(inst.name, line.substr(0, space));
        if (space == std::string_view::npos) {
            EXPECT_EQ(inst.num_src, 0);
        } else {
            ASSERT_EQ(inst.num_src, 1);
            EXPECT_EQ(inst.src[0].value, 5u);
        }
    }
}

}  // namespace
