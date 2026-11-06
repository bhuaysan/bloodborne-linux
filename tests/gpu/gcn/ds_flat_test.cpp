// DS: decoding and text of words encoded by hand from the Sea Islands ISA manual, and a round
// trip of every opcode in the table through llvm-mc.

#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "gpu/gcn/encode.hpp"
#include "gpu/gcn/gcn.hpp"
#include "gpu/gcn/llvm_mc.hpp"

namespace {

using namespace bb::gcn;
using namespace bb::test::gcn;
using Words = std::vector<std::uint32_t>;

struct DsFields {
    unsigned addr = 0, data0 = 0, data1 = 0, vdst = 0;
    unsigned offset = 0;  // offset1 in the high byte, offset0 in the low byte
    bool gds = false;
};

Words ds(unsigned op, DsFields f) {
    return {0x36u << 26 | op << 18 | unsigned{f.gds} << 17 | f.offset,
            f.vdst << 24 | f.data1 << 16 | f.data0 << 8 | f.addr};
}

Instruction one(const Words& words) {
    auto insts = decode(words, Until::code_end);
    EXPECT_EQ(insts.size(), 1u);
    return insts.at(0);
}

TEST(GcnDsFlat, DecodesDsFields) {
    const Instruction inst = one(ds(78, {.addr = 1, .data0 = 2, .data1 = 4, .offset = 0x0803}));
    EXPECT_EQ(inst.format, Format::ds);
    EXPECT_EQ(inst.opcode, 78);
    EXPECT_EQ(inst.name, "ds_write2_b64");
    EXPECT_EQ(inst.size, 2);
    EXPECT_EQ(inst.num_dst, 0);
    ASSERT_EQ(inst.num_src, 3);
    EXPECT_EQ(inst.src[0], (Operand{OperandKind::vector, 1, 1, 0}));
    EXPECT_EQ(inst.src[1], (Operand{OperandKind::vector, 2, 2, 0}));
    EXPECT_EQ(inst.src[2], (Operand{OperandKind::vector, 2, 4, 0}));
    EXPECT_EQ(inst.mem_offset, 0x0803);
    EXPECT_EQ(inst.flags, 0);
    EXPECT_EQ(one(ds(54, {.gds = true})).flags, flag::gds);
    // The next instruction starts after both dwords.
    Words program = ds(54, {});
    program.push_back(sopp(1));
    const auto insts = decode(program);
    ASSERT_EQ(insts.size(), 2u);
    EXPECT_EQ(insts[1].offset, 8u);
}

TEST(GcnDsFlat, Errors) {
    EXPECT_THROW(one(ds(21, {})), GcnError);  // unknown DS opcode
    // A register range past v255.
    EXPECT_THROW(one(ds(255, {.vdst = 253})), GcnError);
    EXPECT_THROW(one(ds(77, {.addr = 0, .data0 = 255})), GcnError);
    // The second dword is missing.
    try {
        decode(Words{ds(54, {})[0]}, Until::code_end);
        FAIL() << "no GcnError";
    } catch (const GcnError& e) {
        EXPECT_EQ(e.offset(), 0u);
        EXPECT_EQ(e.word(), ds(54, {})[0]);
    }
}

struct Case {
    Words words;
    std::string text;
};

const std::vector<Case> cases = {
    // Offsets of every kind, gds, every kind of operand list.
    {ds(0, {.addr = 1, .data0 = 2, .offset = 16, .gds = true}), "ds_add_u32 v1, v2 offset:16 gds"},
    {ds(54, {.addr = 2, .vdst = 1, .offset = 0xffff}), "ds_read_b32 v1, v2 offset:65535"},
    {ds(14, {.addr = 1, .data0 = 2, .data1 = 3, .offset = 0x0804}),
     "ds_write2_b32 v1, v2, v3 offset0:4 offset1:8"},
    {ds(55, {.addr = 3, .vdst = 1, .offset = 0x0200}), "ds_read2_b32 v[1:2], v3 offset1:2"},
    {ds(119, {.addr = 3, .vdst = 4, .offset = 0x0001}), "ds_read2_b64 v[4:7], v3 offset0:1"},
    {ds(111, {.addr = 3, .data0 = 8, .data1 = 10, .vdst = 4}),
     "ds_wrxchg2st64_rtn_b64 v[4:7], v3, v[8:9], v[10:11]"},
    {ds(112, {.addr = 3, .data0 = 4, .data1 = 6, .vdst = 1}),
     "ds_cmpst_rtn_b64 v[1:2], v3, v[4:5], v[6:7]"},
    {ds(53, {.addr = 2, .vdst = 1, .offset = 0x80ff}), "ds_swizzle_b32 v1, v2 offset:0x80ff"},
    {ds(52, {.addr = 2, .data0 = 3, .data1 = 4, .vdst = 1}), "ds_wrap_rtn_b32 v1, v2, v3, v4"},
    {ds(128, {.addr = 1, .offset = 4}), "ds_add_src2_u32 v1 offset:4"},
    {ds(205, {.addr = 7}), "ds_write_src2_b64 v7"},
    {ds(61, {.vdst = 1}), "ds_consume v1"},
    {ds(63, {.addr = 2, .vdst = 1, .offset = 4, .gds = true}),
     "ds_ordered_count v1, v2 offset:4 gds"},
    {ds(25, {.addr = 1, .gds = true}), "ds_gws_init v1 gds"},
    {ds(26, {.offset = 4, .gds = true}), "ds_gws_sema_v offset:4 gds"},
    {ds(20, {}), "ds_nop"},
    {ds(222, {.addr = 1, .data0 = 2}), "ds_write_b96 v1, v[2:4]"},
    {ds(255, {.addr = 255, .vdst = 252}), "ds_read_b128 v[252:255], v255"},
    {ds(126, {.addr = 3, .data0 = 4, .vdst = 1}), "ds_condxchg32_rtn_b64 v[1:2], v3, v[4:5]"},
};

TEST(GcnDsFlat, Text) {
    for (const Case& c : cases) {
        SCOPED_TRACE(c.text);
        EXPECT_EQ(disassemble(one(c.words)), c.text);
    }
}

TEST(GcnDsFlat, AssemblesToTheSameWords) {
    if (!have_llvm_mc()) {
        GTEST_SKIP() << "llvm-mc not found";
    }
    std::vector<std::string> lines;
    for (const Case& c : cases) {
        lines.push_back(c.text);
    }
    const auto assembled = assemble(lines);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        EXPECT_EQ(assembled[i].words, cases[i].words) << cases[i].text;
    }
}

// Encodings that LLVM's gfx7 syntax can't express. The text is still LLVM's syntax.
TEST(GcnDsFlat, EncodingsLlvmCannotExpress) {
    const std::vector<Case> rejected = {
        // GWS and ordered count without gds, ds_nop with it.
        {ds(25, {.addr = 1}), "ds_gws_init v1"},
        {ds(63, {.addr = 2, .vdst = 1}), "ds_ordered_count v1, v2"},
        {ds(20, {.gds = true}), "ds_nop gds"},
        // An opcode with no gfx7 syntax in LLVM.
        {ds(253, {.addr = 3, .data0 = 8, .vdst = 4}), "ds_condxchg32_rtn_b128 v[4:7], v3, v[8:11]"},
    };
    for (const Case& c : rejected) {
        SCOPED_TRACE(c.text);
        EXPECT_EQ(disassemble(one(c.words)), c.text);
        if (have_llvm_mc()) {
            EXPECT_THROW(assemble({c.text}), std::runtime_error);
        }
    }
}

// Every opcode of a table, with distinct registers in every field, through llvm-mc and back.
// `known` is the number of words that decode, `rejected` the opcodes LLVM has no syntax for.
void round_trip(Format format, const std::vector<Words>& all, unsigned known,
                const std::set<std::string>& rejected) {
    std::vector<std::string> lines;
    std::vector<std::uint16_t> opcodes;
    unsigned decoded = 0;
    for (const Words& words : all) {
        try {
            const Instruction inst = decode(words, Until::code_end).at(0);
            ++decoded;
            const std::string text = disassemble(inst);
            if (!rejected.contains(std::string(inst.name))) {
                lines.push_back(text);
                opcodes.push_back(inst.opcode);
            }
        } catch (const GcnError&) {
        }
    }
    EXPECT_EQ(decoded, known);
    if (!have_llvm_mc()) {
        GTEST_SKIP() << "llvm-mc not found";
    }
    const auto assembled = assemble(lines);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        SCOPED_TRACE(lines[i]);
        const Instruction inst = one(assembled[i].words);
        EXPECT_EQ(inst.format, format);
        EXPECT_EQ(inst.opcode, opcodes[i]);
        EXPECT_EQ(disassemble(inst), lines[i]);
    }
}

TEST(GcnDsFlat, RoundTripEveryDsOpcode) {
    std::vector<Words> all;
    for (unsigned op = 0; op < 256; ++op) {
        // gds, which GWS and ordered count need, is valid on every DS opcode but ds_nop.
        all.push_back(ds(op, {.addr = 10, .data0 = 20, .data1 = 30, .vdst = 40, .offset = 0x0804,
                              .gds = op != 20}));
    }
    round_trip(Format::ds, all, 140, {"ds_condxchg32_rtn_b128"});
}

}  // namespace
