// The stage register imports (src/hle/video/gnm_shader.hpp): the dwords each one writes, the
// failures, and a check of the register block layouts against the game's shader files.
#include "hle/video/gnm_shader.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <set>
#include <span>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "common/file.hpp"
#include "shader_recompiler/frontend/decode.h"
#include "support/game.hpp"

namespace {

using namespace bb::hle::video;
using Words = std::vector<std::uint32_t>;

constexpr std::uint32_t poison = 0xdeadbeef;

// SET_SH_REG and SET_CONTEXT_REG of `values` from register offset `offset`.
Words sh(std::uint32_t offset, Words values) {
    Words w{0xc0007600u | static_cast<std::uint32_t>(values.size()) << 16, offset};
    w.insert(w.end(), values.begin(), values.end());
    return w;
}
Words ctx(std::uint32_t offset, Words values) {
    Words w = sh(offset, values);
    w[0] = (w[0] & ~0xff00u) | 0x6900u;
    return w;
}

// The packets followed by a NOP up to `size` dwords, with a zero payload.
Words padded(std::uint32_t size, std::initializer_list<Words> packets) {
    Words w;
    for (const Words& p : packets) {
        w.insert(w.end(), p.begin(), p.end());
    }
    const auto rest = size - static_cast<std::uint32_t>(w.size());
    w.push_back(0xc0001000u | (rest - 2) << 16);
    w.resize(size, 0);
    return w;
}

// Calls `write` on a buffer one dword longer than `size`, poisoned, and returns the first
// `size` dwords; the last one must stay untouched.
template <class F>
Words run(std::uint32_t size, F write) {
    Words buffer(size + 1, poison);
    EXPECT_EQ(write(buffer.data(), size), 0);
    EXPECT_EQ(buffer.back(), poison);
    buffer.pop_back();
    return buffer;
}

TEST(GnmShader, VsWritesItsRegistersAndAppliesTheModifier) {
    const std::uint32_t regs[] = {0x12345, 0, 0x30c00c5, 0x18, 2, 4, 0x400003, 0x203};
    const auto vs = [&](std::uint32_t modifier) {
        return run(vs_shader_dwords, [&](std::uint32_t* cmd, std::uint32_t n) {
            return sceGnmSetVsShader(cmd, n, regs, modifier);
        });
    };
    const auto expected = [](std::uint32_t rsrc1) {
        return padded(vs_shader_dwords, {sh(0x48, {0x12345, 0}), sh(0x4a, {rsrc1, 0x18}),
                                         ctx(0x1b1, {2}), ctx(0x1c3, {4}), ctx(0x207, {0x400003})});
    };
    EXPECT_EQ(vs(0), expected(0x30c00c5));
    EXPECT_EQ(vs(0x40), expected(0xc0045));         // VGPR_COMP_CNT cleared too
    EXPECT_EQ(vs(0x1000380), expected(0x10c0385));  // both fields replaced
}

TEST(GnmShader, PsWritesItsRegistersOrUnbinds) {
    const std::uint32_t regs[] = {0x44, 0, 0xc0041, 0x10, 0, 0x44, 2, 2, 1, 0x1000000, 0x10, 0xff};
    EXPECT_EQ(run(ps_shader_dwords,
                  [&](std::uint32_t* cmd, std::uint32_t n) {
                      return sceGnmSetPsShader(cmd, n, regs);
                  }),
              padded(ps_shader_dwords,
                     {sh(0x08, {0x44, 0}), sh(0x0a, {0xc0041, 0x10}), ctx(0x1c4, {0, 0x44}),
                      ctx(0x1b3, {2, 2}), ctx(0x1b6, {1}), ctx(0x1b8, {0x1000000}),
                      ctx(0x203, {0x10}), ctx(0x08f, {0xff})}));
    EXPECT_EQ(run(ps_shader_dwords,
                  [](std::uint32_t* cmd, std::uint32_t n) {
                      return sceGnmSetPsShader(cmd, n, nullptr);
                  }),
              padded(ps_shader_dwords, {sh(0x08, {0, 0}), ctx(0x203, {0})}));
}

TEST(GnmShader, CsWritesItsRegisters) {
    const std::uint32_t regs[] = {0x34, 0, 0xc0042, 0x1020, 0x20, 4, 1, 0};
    EXPECT_EQ(run(cs_shader_dwords,
                  [&](std::uint32_t* cmd, std::uint32_t n) {
                      return sceGnmSetCsShader(cmd, n, regs);
                  }),
              padded(cs_shader_dwords, {sh(0x20c, {0x34, 0}), sh(0x212, {0xc0042, 0x1020}),
                                        sh(0x207, {0x20, 4, 1})}));
}

TEST(GnmShader, BadArgumentsFailAndWriteNothing) {
    std::uint32_t regs[] = {0x44, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    Words buffer(64, poison);
    std::uint32_t* cmd = buffer.data();
    EXPECT_EQ(sceGnmSetVsShader(cmd, vs_shader_dwords - 1, regs, 0), gnm_shader_error);
    EXPECT_EQ(sceGnmSetVsShader(nullptr, vs_shader_dwords, regs, 0), gnm_shader_error);
    EXPECT_EQ(sceGnmSetVsShader(cmd, vs_shader_dwords, nullptr, 0), gnm_shader_error);
    EXPECT_EQ(sceGnmSetVsShader(cmd, vs_shader_dwords, regs, 0x400), gnm_shader_error);
    EXPECT_EQ(sceGnmSetVsShader(cmd, vs_shader_dwords, regs, 0x4000000), gnm_shader_error);
    EXPECT_EQ(sceGnmSetPsShader(cmd, ps_shader_dwords - 1, nullptr), gnm_shader_error);
    EXPECT_EQ(sceGnmSetCsShader(cmd, cs_shader_dwords, nullptr), gnm_shader_error);
    regs[1] = 0xffffffff;  // the shader files' placeholder for the high address dword
    EXPECT_EQ(sceGnmSetPsShader(cmd, ps_shader_dwords, regs), gnm_shader_error);
    EXPECT_EQ(sceGnmSetCsShader(cmd, cs_shader_dwords, regs), gnm_shader_error);
    EXPECT_EQ(buffer, Words(64, poison));
}

// What a shader file's program uses, from its code.
struct Usage {
    int max_vgpr = -1, max_sgpr = -1, max_attr = -1;
    std::set<unsigned> exports;  // targets: mrt 0-7, mrtz 8, pos 12-15, param 32-63
};

// The operand's width in dwords from its scalar type. shadPS4's decoder doesn't give the register
// count of memory operands (a sample's result, a buffer's data), so the highest register of a
// wider operand can be missed; the check that reads this only needs a lower bound.
int dwords_of(Shader::Gcn::ScalarType type) {
    using Shader::Gcn::ScalarType;
    return type == ScalarType::Uint64 || type == ScalarType::Sint64 || type == ScalarType::Float64
               ? 2
               : 1;
}

Usage usage(std::span<const std::uint32_t> code) {
    using namespace Shader::Gcn;
    Usage u;
    GcnCodeSlice slice(code.data(), code.data() + code.size());
    GcnDecodeContext context;
    while (!slice.atEnd()) {
        const GcnInst inst = context.decodeInstruction(slice);
        const auto note = [&](const InstOperand& op) {
            const int last = static_cast<int>(op.code) + dwords_of(op.type) - 1;
            if (op.field == OperandField::VectorGPR) {
                u.max_vgpr = std::max(u.max_vgpr, last);
            } else if (op.field == OperandField::ScalarGPR) {
                u.max_sgpr = std::max(u.max_sgpr, last);
            }
        };
        // shadPS4's decoder keeps a VOP3 compare's scalar destination in dst[1] and labels dst[0]
        // (the same bits) a vector register.
        const bool vop3_cmp = inst.encoding == InstEncoding::VOP3 &&
                              inst.dst[1].field != OperandField::Undefined &&
                              inst.dst[0].field == OperandField::VectorGPR &&
                              inst.dst[1].type == ScalarType::Uint64 && inst.dst_count == 1;
        if (vop3_cmp) {
            note(inst.dst[1]);
        } else {
            std::for_each(inst.dst.begin(), inst.dst.begin() + inst.dst_count, note);
        }
        std::for_each(inst.src.begin(), inst.src.begin() + inst.src_count, note);
        if (inst.encoding == InstEncoding::VINTRP) {
            u.max_attr = std::max(u.max_attr, static_cast<int>(inst.control.vintrp.attr));
        }
        if (inst.encoding == InstEncoding::EXP) {
            u.exports.insert(static_cast<unsigned>(inst.control.exp.target));
        }
        if (inst.opcode == Opcode::S_ENDPGM) {
            break;
        }
    }
    return u;
}

// Whether nibble i of `value` is non-zero exactly for the targets first + i that `u` exports.
bool nibbles_match(std::uint32_t value, const Usage& u, unsigned first, unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
        if (((value >> 4 * i & 0xf) != 0) != u.exports.contains(first + i)) {
            return false;
        }
    }
    return true;
}

// The registers that `packets` set, by register number.
std::map<std::uint32_t, std::uint32_t> registers(const Words& packets) {
    std::map<std::uint32_t, std::uint32_t> regs;
    for (std::size_t i = 0; i < packets.size();) {
        const std::uint32_t body = (packets[i] >> 16 & 0x3fff) + 1;
        const std::uint32_t op = packets[i] >> 8 & 0xff;
        if (op == 0x76 || op == 0x69) {
            for (std::uint32_t k = 1; k < body; ++k) {
                regs[(op == 0x76 ? 0x2c00 : 0xa000) + packets[i + 1] + k - 1] = packets[i + 1 + k];
            }
        }
        i += body + 1;
    }
    return regs;
}

// Each entry of the shader files gives its register block (from 0x3c, with 0xffffffff for the
// high address dword) to its stage's import, as the game does once it knows the code address.
// The registers written must agree with what the entry's code uses and its header says
// (docs/design/graphics.md, "Stage register imports").
TEST(GnmShaderGame, LayoutsAgreeWithTheShaderFiles) {
    BB_REQUIRE_GAME(game);
    (void)game;
    const std::filesystem::path root = bb::test::analysis_dir() / "shaders";
    BB_REQUIRE_GAME_DATA(root, "tools/shaderbnd");
    std::map<int, int> checked;
    int failures = 0;
    for (const auto& archive : std::filesystem::directory_iterator(root)) {
        if (!std::filesystem::is_directory(archive.path() / "entries")) {
            continue;
        }
        for (const auto& file :
             std::filesystem::recursive_directory_iterator(archive.path() / "entries")) {
            if (!file.is_regular_file()) {
                continue;
            }
            const auto bytes = bb::common::read_file(file.path());
            const auto u32 = [&](std::size_t at) {
                std::uint32_t v = 0;
                std::memcpy(&v, bytes.data() + at, 4);
                return v;
            };
            const std::string_view data(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            const std::size_t footer = data.find("OrbShdr");
            ASSERT_NE(footer, std::string_view::npos) << file.path();
            Words block(16);
            std::memcpy(block.data(), bytes.data() + 0x3c, 64);
            Words code((u32(footer + 8) >> 8) / 4);
            ASSERT_LE(0x34 + block[0] + code.size() * 4, footer) << file.path();
            std::memcpy(code.data(), bytes.data() + 0x34 + block[0], code.size() * 4);
            const Usage use = usage(code);
            const int stage = std::to_integer<int>(bytes[0x2c]);
            EXPECT_EQ(block[1], 0xffffffffu);
            block[1] = 0;
            Words out(64, 0);
            std::int32_t result = gnm_shader_error;
            switch (stage) {
                case 1: result = sceGnmSetVsShader(out.data(), 64, block.data(), 0); break;
                case 2: result = sceGnmSetPsShader(out.data(), 64, block.data()); break;
                case 4: result = sceGnmSetCsShader(out.data(), 64, block.data()); break;
                default: continue;  // ES, GS, HS and LS: the second part of #271
            }
            ASSERT_EQ(result, 0) << file.path();
            const auto r = registers(out);
            // PGM_LO by stage; RSRC1 follows it by two in the graphics stages.
            const std::map<int, std::uint32_t> pgm_lo = {{1, 0x2c48}, {2, 0x2c08}, {4, 0x2e0c}};
            const std::uint32_t lo = pgm_lo.at(stage);
            const std::uint32_t rsrc1 = r.at(stage == 4 ? 0x2e12 : lo + 2);
            bool ok = r.at(lo) == block[0] && r.at(lo + 1) == 0 && rsrc1 == block[2] &&
                      static_cast<int>((rsrc1 & 0x3f) + 1) * 4 > use.max_vgpr &&
                      static_cast<int>((rsrc1 >> 6 & 0xf) + 1) * 8 > use.max_sgpr;
            if (stage == 1) {
                const unsigned params = static_cast<unsigned>(
                    std::count_if(use.exports.begin(), use.exports.end(),
                                  [](unsigned t) { return t >= 32; }));
                ok = ok && nibbles_match(r.at(0xa1c3), use, 12, 4) &&
                     (params == 0 || (r.at(0xa1b1) >> 1 & 0x1f) + 1 == params) &&
                     (r.at(0xa207) != 0) == use.exports.contains(13);
            } else if (stage == 2) {
                const std::uint32_t col = r.at(0xa1c5);
                ok = ok && nibbles_match(col, use, 0, 8) &&
                     (r.at(0xa1c4) != 0) == use.exports.contains(8) &&
                     (r.at(0xa203) & 1) == (r.at(0xa1c4) != 0 ? 1u : 0u) &&
                     r.at(0xa1b3) == r.at(0xa1b4) &&
                     use.max_attr < static_cast<int>(r.at(0xa1b6) & 0x3f);
                const std::uint32_t mask = r.at(0xa08f);
                for (unsigned i = 0; i < 8; ++i) {
                    ok = ok && ((col >> 4 * i & 0xf) != 0) == ((mask >> 4 * i & 0xf) != 0);
                }
            } else if (stage == 4) {
                ok = ok && r.at(0x2e07) == (u32(0x18) & 0xffff) &&
                     r.at(0x2e08) == u32(0x18) >> 16 && r.at(0x2e09) == (u32(0x1c) & 0xffff);
            }
            ++checked[stage];
            if (!ok && ++failures <= 20) {
                ADD_FAILURE() << file.path() << ": registers disagree with the file";
            }
        }
    }
    EXPECT_EQ(failures, 0);
    // Every stage written so far, with the file counts of the full extraction (a truncated one
    // must fail): VS 10585, PS 3209, CS 69.
    EXPECT_EQ(checked, (std::map<int, int>{{1, 10585}, {2, 3209}, {4, 69}}));
}

}  // namespace
