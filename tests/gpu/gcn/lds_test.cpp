// Which DS instructions the translator handles, by name (src/gpu/gcn/lds.hpp), checked against
// every DS opcode the decoder knows.

#include <array>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "gpu/gcn/gcn.hpp"
#include "gpu/gcn/lds.hpp"

namespace {

using namespace bb::gcn::detail;

TEST(GcnLds, Reads) {
    auto d = classify_ds("ds_read_b32");
    ASSERT_TRUE(d);
    EXPECT_EQ(d->kind, DsKind::read);
    EXPECT_EQ(d->bytes, 4);
    EXPECT_FALSE(d->pair);
    d = classify_ds("ds_read_i8");
    ASSERT_TRUE(d);
    EXPECT_EQ(d->bytes, 1);
    EXPECT_TRUE(d->is_signed);
    EXPECT_FALSE(classify_ds("ds_read_u16")->is_signed);
    d = classify_ds("ds_read2st64_b64");
    ASSERT_TRUE(d);
    EXPECT_TRUE(d->pair);
    EXPECT_TRUE(d->stride64);
    EXPECT_EQ(d->bytes, 8);
    EXPECT_EQ(classify_ds("ds_read_b128")->bytes, 16);
}

TEST(GcnLds, WritesAndAtomics) {
    EXPECT_EQ(classify_ds("ds_write_b8")->bytes, 1);
    EXPECT_TRUE(classify_ds("ds_write2_b64")->pair);
    auto d = classify_ds("ds_min_rtn_i32");
    ASSERT_TRUE(d);
    EXPECT_EQ(d->kind, DsKind::atomic);
    EXPECT_EQ(d->op, DsAtomic::smin);
    EXPECT_TRUE(d->returns);
    EXPECT_EQ(classify_ds("ds_min_u32")->op, DsAtomic::umin);
    EXPECT_FALSE(classify_ds("ds_min_u32")->returns);
    EXPECT_EQ(classify_ds("ds_wrxchg_rtn_b32")->op, DsAtomic::exchange);
    EXPECT_EQ(classify_ds("ds_cmpst_b32")->op, DsAtomic::cmpst);
    EXPECT_EQ(classify_ds("ds_nop")->kind, DsKind::nop);
}

// What the translator doesn't handle is nothing, never a guess.
TEST(GcnLds, UnhandledNames) {
    for (const char* name :
         {"ds_add_u64", "ds_min_f32", "ds_cmpst_f32", "ds_add_src2_u32", "ds_wrxchg_b32",
          "ds_wrxchg2_rtn_b32", "ds_swizzle_b32", "ds_gws_barrier", "ds_append", "ds_consume",
          "ds_condxchg32_rtn_b64", "ds_read_b8", "ds_read2_b96", "ds_write2_b128", "v_mov_b32",
          "ds_", "ds_add", ""}) {
        EXPECT_FALSE(classify_ds(name)) << name;
    }
}

// The names of the decoder's DS opcodes that are handled: 12 reads, 10 writes, 29 atomics (14
// without and 15 with a return value) and ds_nop.
TEST(GcnLds, HandledOpcodes) {
    std::set<std::string> seen;
    for (unsigned op = 0; op < 256; ++op) {
        const std::uint32_t w0 = 0xd8000000u | op << 18;
        const std::array<std::uint32_t, 2> code = {w0, 0};
        std::vector<bb::gcn::Instruction> program;
        try {
            program = bb::gcn::decode(code, bb::gcn::Until::code_end);
        } catch (const bb::gcn::GcnError&) {
            continue;
        }
        if (classify_ds(program.at(0).name)) {
            seen.insert(std::string(program[0].name));
        }
    }
    EXPECT_EQ(seen.size(), 52u);
}

}  // namespace
