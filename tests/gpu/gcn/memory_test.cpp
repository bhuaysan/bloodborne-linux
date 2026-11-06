// MUBUF, MTBUF, MIMG and EXP: decoding and LLVM's syntax for words encoded by hand from the Sea
// Islands ISA manual, and a round trip through llvm-mc for every opcode and export target.

#include <cstdint>
#include <format>
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

Instruction one(const std::vector<std::uint32_t>& words) {
    auto insts = decode(words, Until::code_end);
    EXPECT_EQ(insts.size(), 1u);
    return insts.at(0);
}

std::string text(const std::vector<std::uint32_t>& words) {
    return disassemble(one(words));
}

std::string error(const std::vector<std::uint32_t>& words) {
    try {
        decode(words, Until::code_end);
    } catch (const GcnError& e) {
        return e.what();
    }
    return "no error";
}

Operand v(unsigned first, std::uint8_t dwords = 1) {
    return {OperandKind::vector, dwords, static_cast<std::uint16_t>(first), 0};
}

TEST(GcnMemory, Mubuf) {
    // A load writes vdata, with a status dword after the data when tfe is set.
    Instruction inst = one(mubuf(12, 1, 2, 4, 3, 0x123, buf::offen | buf::glc | buf::tfe));
    EXPECT_EQ(inst.format, Format::mubuf);
    EXPECT_EQ(inst.size, 2);
    EXPECT_EQ(inst.num_dst, 1);
    EXPECT_EQ(inst.dst[0], v(1, 2));
    ASSERT_EQ(inst.num_src, 3);
    EXPECT_EQ(inst.src[0], v(2));
    EXPECT_EQ(inst.src[1], (Operand{OperandKind::scalar, 4, 4, 0}));
    EXPECT_EQ(inst.src[2], (Operand{OperandKind::scalar, 1, 3, 0}));
    EXPECT_EQ(inst.mem_offset, 0x123);
    EXPECT_EQ(inst.flags, flag::offen | flag::glc | flag::tfe);

    // Stores and atomics read vdata. A load into LDS has none.
    inst = one(mubuf(81, 4, 0, 0, 241));  // buffer_atomic_cmpswap_x2
    EXPECT_EQ(inst.num_dst, 0);
    ASSERT_EQ(inst.num_src, 4);
    EXPECT_EQ(inst.src[0], v(4, 4));
    EXPECT_EQ(inst.src[1], (Operand{OperandKind::off, 0, 0, 0}));
    EXPECT_EQ(inst.src[3], (Operand{OperandKind::constant, 1, 241, 0xbf000000}));  // -0.5
    inst = one(mubuf(12, 0, 0, 4, 1, 0, buf::lds));
    EXPECT_EQ(inst.num_dst, 0);
    EXPECT_EQ(inst.num_src, 3);
}

TEST(GcnMemory, Errors) {
    EXPECT_NE(error(mubuf(52, 1, 0, 4, 1)).find("unknown MUBUF opcode 52 at offset 0x0"),
              std::string::npos);
    EXPECT_NE(error({mubuf(12, 1, 0, 4, 1)[0]}).find("second dword past the end"),
              std::string::npos);
    EXPECT_NE(error(mubuf(28, 1, 0, 4, 1, 0, buf::lds)).find("lds bit on a store"),
              std::string::npos);
    EXPECT_NE(error(mubuf(14, 253, 0, 4, 1)).find("VGPR range v253 with 4 dwords past v255"),
              std::string::npos);
    EXPECT_NE(error(mubuf(12, 1, 0, 4, 255)).find("invalid soffset operand 255"),  // no literal
              std::string::npos);
    EXPECT_NE(error(mubuf(12, 1, 0, 104, 1)).find("invalid 4-dword descriptor operand 104"),
              std::string::npos);
}

struct Case {
    std::vector<std::uint32_t> words;
    std::string text;
};

// Address modes, fields, the soffset forms, a load into LDS.
const std::vector<Case> cases = {
    {mubuf(12, 1, 0, 4, 1), "buffer_load_dword v1, off, s[4:7], s1"},
    {mubuf(12, 1, 2, 4, 1, 4095, buf::offen | buf::glc | buf::slc | buf::tfe),
     "buffer_load_dword v[1:2], v2, s[4:7], s1 offen offset:4095 glc slc tfe"},
    {mubuf(3, 1, 2, 8, 128, 0, buf::idxen | buf::offen),
     "buffer_load_format_xyzw v[1:4], v[2:3], s[8:11], 0 idxen offen"},
    {mubuf(12, 255, 254, 4, 128, 0, buf::addr64),
     "buffer_load_dword v255, v[254:255], s[4:7], 0 addr64"},
    {mubuf(28, 1, 0, 116, code::m0), "buffer_store_dword v1, off, ttmp[4:7], m0"},
    {mubuf(31, 1, 0, 4, 242, 8), "buffer_store_dwordx3 v[1:3], off, s[4:7], 1.0 offset:8"},
    {mubuf(50, 1, 2, 4, 193, 0, buf::offen | buf::glc),
     "buffer_atomic_add v1, v2, s[4:7], -1 offen glc"},
    {mubuf(62, 2, 0, 4, code::vcc + 1), "buffer_atomic_fcmpswap v[2:3], off, s[4:7], vcc_hi"},
    {mubuf(12, 0, 2, 4, 1, 8, buf::offen | buf::lds),
     "buffer_load_dword v2, s[4:7], s1 offen offset:8 lds"},
    {mubuf(112, 0, 0, 0, 0), "buffer_wbinvl1_vol"},
};

TEST(GcnMemory, Text) {
    for (const Case& c : cases) {
        SCOPED_TRACE(c.text);
        EXPECT_EQ(text(c.words), c.text);
    }
}

// The words llvm-mc assembles from the text of each case.
void expect_assembles(const std::vector<Case>& cases) {
    std::vector<std::string> lines;
    for (const Case& c : cases) {
        lines.push_back(c.text);
    }
    const auto assembled = assemble(lines);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        EXPECT_EQ(assembled[i].words, cases[i].words) << cases[i].text;
        EXPECT_EQ(assembled[i].text, cases[i].text);
    }
}

TEST(GcnMemory, AssemblesToTheSameWords) {
    if (!have_llvm_mc()) {
        GTEST_SKIP() << "llvm-mc not found";
    }
    expect_assembles(cases);
}

// Field combinations that LLVM's syntax can't express. The decoder accepts them, and llvm-mc
// rejects the text or encodes it differently.
TEST(GcnMemory, EncodingsLlvmCannotExpress) {
    const std::vector<Case> differ = {
        {mubuf(28, 1, 0, 4, 1, 0, buf::tfe), "buffer_store_dword v1, off, s[4:7], s1 tfe"},
        {mubuf(50, 1, 0, 4, 1, 0, buf::tfe), "buffer_atomic_add v1, off, s[4:7], s1 tfe"},
        {mubuf(12, 1, 2, 4, 1, 0, buf::addr64 | buf::offen),
         "buffer_load_dword v1, v[2:3], s[4:7], s1 offen addr64"},
        {mubuf(12, 255, 0, 4, 1, 0, buf::lds | buf::tfe),
         "buffer_load_dword off, s[4:7], s1 lds tfe"},
        {mubuf(13, 0, 0, 4, 1, 0, buf::lds), "buffer_load_dwordx2 off, s[4:7], s1 lds"},
        // Fields that an instruction doesn't use don't appear in the text.
        {mubuf(113, 1, 2, 4, 1), "buffer_wbinvl1"},
        {mubuf(14, 253, 0, 4, 1, 0, buf::lds), "buffer_load_dwordx4 off, s[4:7], s1 lds"},
    };
    for (const Case& c : differ) {
        SCOPED_TRACE(c.text);
        EXPECT_EQ(text(c.words), c.text);
        if (have_llvm_mc()) {
            try {
                EXPECT_NE(assemble({c.text}).at(0).words, c.words);
            } catch (const std::runtime_error&) {
                // rejected
            }
        }
    }
}

// Every opcode of the table, with typical operands, through llvm-mc and back.
TEST(GcnMemory, RoundTripMubuf) {
    std::vector<Case> all;
    for (unsigned op = 0; op < 128; ++op) {
        // The cache invalidations have no operands, so their fields are 0.
        const auto w = op >= 112 ? mubuf(op, 0, 0, 0, 0) : mubuf(op, 1, 6, 4, 2, 4, buf::offen);
        try {
            all.push_back({w, text(w)});
        } catch (const GcnError&) {
        }
    }
    EXPECT_EQ(all.size(), 56u);
    if (!have_llvm_mc()) {
        GTEST_SKIP() << "llvm-mc not found";
    }
    expect_assembles(all);
}

TEST(GcnMemory, Mtbuf) {
    // Decoded like MUBUF, with the formats in their own members.
    Instruction inst = one(mtbuf(3, 1, 2, 8, 128, 0x7ff, 10, 4, buf::offen | buf::glc));
    EXPECT_EQ(inst.format, Format::mtbuf);
    EXPECT_EQ(inst.name, "tbuffer_load_format_xyzw");
    EXPECT_EQ(inst.size, 2);
    EXPECT_EQ(inst.dst[0], v(1, 4));
    ASSERT_EQ(inst.num_src, 3);
    EXPECT_EQ(inst.src[0], v(2));
    EXPECT_EQ(inst.mem_offset, 0x7ff);
    EXPECT_EQ(inst.flags, flag::offen | flag::glc);
    EXPECT_EQ(inst.dfmt, 10);
    EXPECT_EQ(inst.nfmt, 4);
    inst = one(mtbuf(5, 4, 0, 4, 1, 0, 1, 0));  // a store reads vdata
    EXPECT_EQ(inst.num_dst, 0);
    EXPECT_EQ(inst.src[0], v(4, 2));
    // Bit 16 is part of the opcode, not an lds bit.
    EXPECT_EQ(one(mtbuf(1, 1, 0, 4, 1, 0, 1, 0)).flags, 0);

    EXPECT_NE(error({mtbuf(0, 1, 0, 4, 1, 0, 1, 0)[0]}).find("second dword past the end"),
              std::string::npos);
    EXPECT_NE(error(mtbuf(3, 253, 0, 4, 1, 0, 1, 0)).find("v253 with 4 dwords past v255"),
              std::string::npos);
}

// The formats print after soffset. LLVM leaves out the data format 8 and numeric format UNORM.
const std::vector<Case> mtbuf_cases = {
    {mtbuf(0, 1, 0, 4, 1, 0, 1, 0), "tbuffer_load_format_x v1, off, s[4:7], s1"},
    {mtbuf(1, 1, 2, 4, 1, 4095, 11, 7, buf::offen | buf::glc | buf::slc),
     "tbuffer_load_format_xy v[1:2], v2, s[4:7], s1 "
     "format:[BUF_DATA_FORMAT_32_32,BUF_NUM_FORMAT_FLOAT] offen offset:4095 glc slc"},
    {mtbuf(2, 1, 2, 4, 128, 0, 1, 5, buf::offen),
     "tbuffer_load_format_xyz v[1:3], v2, s[4:7], 0 format:[BUF_NUM_FORMAT_SINT] offen"},
    {mtbuf(4, 1, 2, 4, 1, 0, 2, 0, buf::idxen),
     "tbuffer_store_format_x v1, v2, s[4:7], s1 format:[BUF_DATA_FORMAT_16] idxen"},
    {mtbuf(7, 1, 2, 4, 1, 0, 0, 0, buf::addr64),
     "tbuffer_store_format_xyzw v[1:4], v[2:3], s[4:7], s1 "
     "format:[BUF_DATA_FORMAT_INVALID] addr64"},
    {mtbuf(7, 1, 2, 4, 1, 8, 15, 6, buf::idxen | buf::offen),
     "tbuffer_store_format_xyzw v[1:4], v[2:3], s[4:7], s1 "
     "format:[BUF_DATA_FORMAT_RESERVED_15,BUF_NUM_FORMAT_SNORM_OGL] idxen offen offset:8"},
};

TEST(GcnMemory, MtbufText) {
    for (const Case& c : mtbuf_cases) {
        SCOPED_TRACE(c.text);
        EXPECT_EQ(text(c.words), c.text);
    }
}

TEST(GcnMemory, MtbufAssemblesToTheSameWords) {
    if (!have_llvm_mc()) {
        GTEST_SKIP() << "llvm-mc not found";
    }
    expect_assembles(mtbuf_cases);
}

// Every opcode and every data and numeric format through llvm-mc and back.
TEST(GcnMemory, RoundTripMtbuf) {
    std::vector<Case> all;
    for (unsigned op = 0; op < 8; ++op) {
        const auto w = mtbuf(op, 1, 2, 4, 1, 4, 3, 1, buf::offen);
        all.push_back({w, text(w)});
    }
    for (unsigned dfmt = 0; dfmt < 16; ++dfmt) {
        for (unsigned nfmt = 0; nfmt < 8; ++nfmt) {
            const auto w = mtbuf(0, 1, 0, 4, 1, 0, dfmt, nfmt);
            all.push_back({w, text(w)});
        }
    }
    if (!have_llvm_mc()) {
        GTEST_SKIP() << "llvm-mc not found";
    }
    expect_assembles(all);
}

TEST(GcnMemory, Export) {
    Instruction inst = one(xport(0xf, 3, 4, 5, 6, 7, xp::done | xp::vm));
    EXPECT_EQ(inst.format, Format::exp);
    EXPECT_EQ(inst.size, 2);
    EXPECT_EQ(inst.num_dst, 0);
    EXPECT_EQ(inst.mem_offset, 3);  // the target
    EXPECT_EQ(inst.flags, flag::done | flag::vm);
    ASSERT_EQ(inst.num_src, 4);
    EXPECT_EQ(inst.src[0], v(4));
    EXPECT_EQ(inst.src[3], v(7));
    // A clear enable bit gives `off`. Compressed sources share a register per pair.
    inst = one(xport(0x5, 0, 4, 5, 6, 7));
    EXPECT_EQ(inst.src[1], (Operand{OperandKind::off, 0, 0, 0}));
    EXPECT_EQ(inst.src[2], v(6));
    inst = one(xport(0xd, 0, 4, 5, 6, 7, xp::compr));
    EXPECT_EQ(inst.flags, flag::compr);
    EXPECT_EQ(inst.src[0], v(4));
    EXPECT_EQ(inst.src[1], (Operand{OperandKind::off, 0, 0, 0}));
    EXPECT_EQ(inst.src[2], v(5));
    EXPECT_EQ(inst.src[3], v(5));

    for (unsigned t : {10u, 11u, 16u, 31u}) {
        const auto message = std::format("reserved export target {}", t);
        EXPECT_NE(error(xport(0xf, t, 0, 1, 2, 3)).find(message), std::string::npos);
    }
    EXPECT_NE(error({xport(0xf, 0, 0, 1, 2, 3)[0]}).find("second dword past the end"),
              std::string::npos);
}

const std::vector<Case> export_cases = {
    {xport(0xf, 0, 0, 1, 0, 0, xp::done | xp::compr | xp::vm),
     "exp mrt0 v0, v0, v1, v1 done compr vm"},
    {xport(0x3, 7, 0, 0, 0, 0, xp::compr), "exp mrt7 v0, v0, off, off compr"},
    {xport(0xc, 1, 0, 9, 0, 0, xp::compr), "exp mrt1 off, off, v9, v9 compr"},
    {xport(0xf, 12, 0, 1, 2, 3, xp::done), "exp pos0 v0, v1, v2, v3 done"},
    {xport(0x5, 63, 0, 0, 255, 0, xp::vm), "exp param31 v0, off, v255, off vm"},
    {xport(0x0, 8, 0, 0, 0, 0), "exp mrtz off, off, off, off"},
    {xport(0xf, 9, 0, 1, 2, 3), "exp null v0, v1, v2, v3"},
};

TEST(GcnMemory, ExportText) {
    for (const Case& c : export_cases) {
        SCOPED_TRACE(c.text);
        EXPECT_EQ(text(c.words), c.text);
    }
}

TEST(GcnMemory, ExportAssemblesToTheSameWords) {
    if (!have_llvm_mc()) {
        GTEST_SKIP() << "llvm-mc not found";
    }
    expect_assembles(export_cases);
}

// Every valid target, with and without the flags.
TEST(GcnMemory, RoundTripExport) {
    std::vector<Case> all;
    for (unsigned t = 0; t < 64; ++t) {
        for (std::uint64_t flags : {std::uint64_t{0}, xp::done | xp::vm}) {
            const auto w = xport(0xf, t, 1, 2, 3, 4, flags);
            if (t == 10 || t == 11 || (t >= 16 && t < 32)) {
                EXPECT_NE(error(w), "no error");
                continue;
            }
            all.push_back({w, text(w)});
        }
    }
    EXPECT_EQ(all.size(), 2u * 46);
    if (!have_llvm_mc()) {
        GTEST_SKIP() << "llvm-mc not found";
    }
    expect_assembles(all);
}

// Encodings that llvm-mc rejects or encodes differently: tfe on a typed load, and a compressed
// export whose enable bits are not set in pairs (LLVM sets them in pairs).
TEST(GcnMemory, TypedAndExportEncodingsLlvmCannotExpress) {
    const std::vector<Case> differ = {
        {mtbuf(1, 1, 2, 4, 1, 0, 1, 0, buf::offen | buf::tfe),
         "tbuffer_load_format_xy v[1:3], v2, s[4:7], s1 offen tfe"},
        {xport(0x1, 0, 4, 5, 6, 7, xp::compr), "exp mrt0 v4, off, off, off compr"},
    };
    for (const Case& c : differ) {
        SCOPED_TRACE(c.text);
        EXPECT_EQ(text(c.words), c.text);
        if (have_llvm_mc()) {
            try {
                EXPECT_NE(assemble({c.text}).at(0).words, c.words);
            } catch (const std::runtime_error&) {
                // rejected
            }
        }
    }
}

TEST(GcnMemory, Image) {
    // A sample: vdata is the destination, one register per dmask bit and one for the status
    // under tfe. The resource is 8 SGPRs and the sampler 4.
    Instruction inst = one(mimg(32, 1, 4, 8, 12, 0x5, img::tfe | img::glc | img::slc | img::da));
    EXPECT_EQ(inst.format, Format::mimg);
    EXPECT_EQ(inst.size, 2);
    EXPECT_EQ(inst.opcode, 32);
    EXPECT_EQ(inst.num_dst, 1);
    EXPECT_EQ(inst.dst[0], v(1, 3));
    ASSERT_EQ(inst.num_src, 3);
    EXPECT_EQ(inst.src[0], v(4));
    EXPECT_EQ(inst.src[1], (Operand{OperandKind::scalar, 8, 8, 0}));
    EXPECT_EQ(inst.src[2], (Operand{OperandKind::scalar, 4, 12, 0}));
    EXPECT_EQ(inst.mem_offset, 0x5);  // the dmask
    EXPECT_EQ(inst.flags, flag::tfe | flag::glc | flag::slc | flag::da);

    // A load has no sampler, and dmask 0 still returns one register. A store and an atomic read
    // vdata. A gather returns four registers. The address is the shortest LLVM accepts.
    inst = one(mimg(0, 2, 4, 8, 0, 0, img::unorm | img::r128 | img::lwe));
    EXPECT_EQ(inst.dst[0], v(2));
    EXPECT_EQ(inst.num_src, 2);
    EXPECT_EQ(inst.flags, flag::unorm | flag::r128 | flag::lwe);
    inst = one(mimg(8, 2, 4, 8, 0, 0xf));
    EXPECT_EQ(inst.num_dst, 0);
    EXPECT_EQ(inst.src[0], v(2, 4));
    inst = one(mimg(17, 2, 4, 8, 0, 0x3));  // image_atomic_add
    EXPECT_EQ(inst.src[0], v(2, 2));
    inst = one(mimg(68, 2, 4, 8, 12, 0x2, img::tfe));  // image_gather4_l
    EXPECT_EQ(inst.dst[0], v(2, 5));
    inst = one(mimg(111, 2, 4, 8, 12, 0xf));  // image_sample_c_cd_cl_o
    EXPECT_EQ(inst.src[0], v(4, 4));

    EXPECT_NE(error(mimg(6, 1, 4, 8, 0, 1)).find("unknown MIMG opcode 6 at offset 0x0"),
              std::string::npos);
    EXPECT_NE(error(mimg(66, 1, 4, 8, 12, 1)).find("unknown MIMG opcode 66"), std::string::npos);
    EXPECT_NE(error({mimg(0, 1, 4, 8, 0, 1)[0]}).find("second dword past the end"),
              std::string::npos);
    EXPECT_NE(error(mimg(0, 253, 4, 8, 0, 0xf)).find("VGPR range v253 with 4 dwords"),
              std::string::npos);
    EXPECT_NE(error(mimg(0, 1, 4, 104, 0, 1)).find("invalid 8-dword descriptor operand 104"),
              std::string::npos);
    EXPECT_NE(error(mimg(32, 1, 4, 8, 124, 1)).find("invalid 4-dword descriptor operand 124"),
              std::string::npos);
}

const std::vector<Case> image_cases = {
    // The starting point of the design: a sample with all four components.
    {mimg(32, 0, 4, 8, 12, 0xf), "image_sample v[0:3], v4, s[8:15], s[12:15] dmask:0xf"},
    {mimg(0, 1, 4, 8, 0, 0), "image_load v1, v4, s[8:15]"},
    {mimg(0, 1, 4, 96, 0, 0x1), "image_load v1, v4, s[96:103] dmask:0x1"},
    // The flags print in LLVM's order, and r128 still prints eight SGPRs.
    {mimg(32, 1, 4, 8, 12, 0xf,
          img::unorm | img::glc | img::slc | img::r128 | img::tfe | img::lwe | img::da),
     "image_sample v[1:5], v4, s[8:15], s[12:15] dmask:0xf unorm glc slc r128 tfe lwe da"},
    {mimg(8, 4, 8, 16, 0, 0x7), "image_store v[4:6], v8, s[16:23] dmask:0x7"},
    {mimg(16, 0, 4, 8, 0, 0x3, img::glc),
     "image_atomic_cmpswap v[0:1], v4, s[8:15] dmask:0x3 glc"},
    {mimg(14, 2, 4, 8, 0, 0xf), "image_get_resinfo v[2:5], v4, s[8:15] dmask:0xf"},
    {mimg(104, 0, 4, 8, 12, 0xf), "image_sample_cd v[0:3], v[4:5], s[8:15], s[12:15] dmask:0xf"},
    {mimg(93, 0, 4, 8, 12, 0x4),
     "image_gather4_c_b_o v[0:3], v[4:7], s[8:15], s[12:15] dmask:0x4"},
    {mimg(96, 0, 4, 8, 12, 0x3), "image_get_lod v[0:1], v4, s[8:15], s[12:15] dmask:0x3"},
    {mimg(39, 8, 252, 96, 100, 0x1),
     "image_sample_lz v8, v252, s[96:103], s[100:103] dmask:0x1"},
};

TEST(GcnMemory, ImageText) {
    for (const Case& c : image_cases) {
        SCOPED_TRACE(c.text);
        EXPECT_EQ(text(c.words), c.text);
    }
}

TEST(GcnMemory, ImageAssemblesToTheSameWords) {
    if (!have_llvm_mc()) {
        GTEST_SKIP() << "llvm-mc not found";
    }
    expect_assembles(image_cases);
}

// Every opcode with typical operands through llvm-mc and back: each kind of dmask, and tfe.
TEST(GcnMemory, RoundTripImage) {
    std::vector<Case> all;
    for (unsigned op = 0; op < 128; ++op) {
        try {
            const bool atomic = op >= 15 && op < 32, gather = op >= 64 && op < 96;
            const unsigned dmask = atomic ? 3 : gather ? 2 : 0xb;
            const unsigned ssamp = op >= 32 ? 12 : 0;  // the sampling opcodes
            for (std::uint64_t flags : {std::uint64_t{0}, img::tfe | img::glc | img::da}) {
                const auto w = mimg(op, 1, 4, 8, ssamp, dmask, flags);
                all.push_back({w, text(w)});
            }
        } catch (const GcnError&) {
        }
    }
    EXPECT_EQ(all.size(), 2u * 93);
    if (!have_llvm_mc()) {
        GTEST_SKIP() << "llvm-mc not found";
    }
    expect_assembles(all);
}

// Every dmask of a load and a sample through llvm-mc, and the other flag combinations.
TEST(GcnMemory, RoundTripImageDmaskAndFlags) {
    std::vector<Case> all;
    for (unsigned dmask = 0; dmask < 16; ++dmask) {
        for (unsigned op : {0u, 8u, 32u}) {
            const auto w = mimg(op, 1, 4, 8, op == 32 ? 12 : 0, dmask, dmask & 1 ? img::tfe : 0);
            all.push_back({w, text(w)});
        }
    }
    for (unsigned flags = 0; flags < 128; ++flags) {
        const std::uint64_t f = (flags & 1 ? img::unorm : 0) | (flags & 2 ? img::glc : 0) |
                                (flags & 4 ? img::slc : 0) | (flags & 8 ? img::r128 : 0) |
                                (flags & 16 ? img::tfe : 0) | (flags & 32 ? img::lwe : 0) |
                                (flags & 64 ? img::da : 0);
        const auto w = mimg(37, 1, 4, 8, 12, 1, f);
        all.push_back({w, text(w)});
    }
    if (!have_llvm_mc()) {
        GTEST_SKIP() << "llvm-mc not found";
    }
    expect_assembles(all);
}

// Encodings that llvm-mc rejects: a gather4 whose dmask has more or fewer than one bit, and an
// atomic with a dmask other than 1 or 3 (3 or 15 for the compare-and-swap atomics).
// The compare-and-swap atomics take 3 or 15 as dmask, which the round trip over every opcode
// (dmask 3) doesn't reach.
TEST(GcnMemory, RoundTripImageCmpswapDmask15) {
    std::vector<Case> all;
    for (unsigned op : {16u, 29u}) {
        const auto w = mimg(op, 0, 4, 8, 0, 0xf, img::glc);
        all.push_back({w, text(w)});
    }
    if (!have_llvm_mc()) {
        GTEST_SKIP() << "llvm-mc not found";
    }
    expect_assembles(all);
}

TEST(GcnMemory, ImageEncodingsLlvmCannotExpress) {
    const std::vector<Case> differ = {
        {mimg(64, 0, 4, 8, 12, 0x3), "image_gather4 v[0:3], v4, s[8:15], s[12:15] dmask:0x3"},
        {mimg(64, 0, 4, 8, 12, 0), "image_gather4 v[0:3], v4, s[8:15], s[12:15]"},
        {mimg(17, 0, 4, 8, 0, 0x7), "image_atomic_add v[0:2], v4, s[8:15] dmask:0x7"},
        {mimg(16, 0, 4, 8, 0, 0x1), "image_atomic_cmpswap v0, v4, s[8:15] dmask:0x1"},
        {mimg(16, 0, 4, 8, 0, 0x7), "image_atomic_cmpswap v[0:2], v4, s[8:15] dmask:0x7"},
        {mimg(29, 0, 4, 8, 0, 0x1), "image_atomic_fcmpswap v0, v4, s[8:15] dmask:0x1"},
    };
    for (const Case& c : differ) {
        SCOPED_TRACE(c.text);
        EXPECT_EQ(text(c.words), c.text);
        if (have_llvm_mc()) {
            EXPECT_THROW(assemble({c.text}), std::runtime_error);
        }
    }
}

}  // namespace
