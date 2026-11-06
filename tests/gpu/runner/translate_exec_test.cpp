// Execution tests of the translator on lavapipe: our GCN programs in tests/gpu/programs/, run as
// one workgroup of 64 lanes, against values worked out from the ISA manual. The modules are the
// ones that GcnTranslate.ProgramsPassSpirvVal validates (same programs, same key).

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "gpu/gcn/programs.hpp"
#include "gpu/gcn/translate.hpp"
#include "gpu/runner/compute_runner.hpp"

namespace {

using namespace bb::test::vk;

constexpr std::uint64_t base = 0x12'0000'0000;  // guest address of the output
constexpr std::uint32_t lanes = 64;
constexpr std::uint64_t data_offset = 0x1400;   // the memory programs' table, after their output
constexpr std::uint64_t store_offset = 0x1c00;  // and the buffer they store to
constexpr std::uint32_t data_word(std::uint32_t i) { return 0xd0000 + i; }

// The k-th operation of the SALU bitwise family (and, or, xor, andn2, orn2, nand, nor, xnor) on
// two words (or the lowest bit of two).
std::uint32_t bitwise(unsigned k, std::uint32_t a, std::uint32_t b) {
    const std::uint32_t ops[] = {a & b, a | b, a ^ b, a & ~b, a | ~b, ~(a & b), ~(a | b), ~(a ^ b)};
    return ops[k];
}

class TranslateExec : public testing::Test {
protected:
    void SetUp() override {
        std::string why;
        runner_ = ComputeRunner::create(why);
        if (!runner_) {
            GTEST_SKIP() << why;
        }
    }

    // Runs the program in one workgroup of `threads` lanes with user_data = {output address, then
    // `inputs`} and returns the first `words` dwords of the guest memory, at least 0x3000 bytes
    // with the table of the memory programs (data_word) at data_offset. `pgm_rsrc1` is the key's
    // float mode.
    std::vector<std::uint32_t> run(std::string_view name, std::vector<std::uint32_t> inputs,
                                   std::uint32_t words, std::uint32_t pgm_rsrc1 = 0,
                                   std::uint32_t threads = lanes) {
        const auto translation = bb::gcn::translate(
            bb::test::gcn::program_words(name), {},
            {.pgm_rsrc1 = pgm_rsrc1,
             .pgm_rsrc2 = bb::test::gcn::program_pgm_rsrc2(name),
             .num_threads = {threads, 1, 1}});
        auto memory = runner_->guest_memory(base, std::max<std::size_t>(0x3000, words * 4));
        for (std::uint32_t i = 0; i < 0x200; ++i) {
            const std::uint32_t word = data_word(i);
            std::memcpy(memory.data() + data_offset + 4 * i, &word, 4);
        }
        DrawBlock draw;
        draw.user_data = {static_cast<std::uint32_t>(base), static_cast<std::uint32_t>(base >> 32)};
        std::ranges::copy(inputs, draw.user_data.begin() + 2);
        runner_->run(translation.spirv, draw, {1, 1, 1});
        std::vector<std::uint32_t> out(words);
        std::memcpy(out.data(), memory.data(), words * 4);
        return out;
    }

    std::unique_ptr<ComputeRunner> runner_;
};

TEST_F(TranslateExec, ExecLaneBits) {
    constexpr std::uint32_t value = 0xa5a5'5a5a, exec_lo = 0x8000'00f1, exec_hi = 0xf0f0'f0f0;
    const auto out = run("exec_lanes", {value, exec_lo}, 2 * lanes);
    for (std::uint32_t i = 0; i < lanes; ++i) {
        SCOPED_TRACE(testing::Message() << "lane " << i);
        const bool active = ((i < 32 ? exec_lo >> i : exec_hi >> (i - 32)) & 1) != 0;
        const std::uint32_t x = value ^ (4 * i);
        const std::uint32_t v6 = active ? x & 0xffff : x;
        EXPECT_EQ(out[i], active ? v6 : 0);           // stored by the active lanes only
        EXPECT_EQ(out[lanes + i], i >= 2 ? v6 : 0);  // EXEC = -4
    }
}

// The other writes to EXEC: from EXEC itself, the constants 0 and -1, a float
// constant (the f64 1.0 is 0x3ff0'0000'0000'0000, lanes 52-61), and a 32-bit ALU write of one
// half (EXEC = -1 first, so the high half keeps lanes 32-63).
TEST_F(TranslateExec, ExecWrites) {
    constexpr std::uint32_t mask_lo = 0x0000'f00f, mask_hi = 0xc300'0001;
    constexpr std::uint64_t mask = (std::uint64_t{mask_hi} << 32) | mask_lo;
    constexpr std::uint64_t stage_exec[] = {
        mask, 0, ~std::uint64_t{0}, 0x3ff0'0000ull << 32, (~std::uint64_t{0} << 32) | 0x111,
    };
    const auto out = run("exec_writes", {mask_lo, mask_hi, 0x100}, 5 * lanes);
    for (std::uint32_t stage = 0; stage < 5; ++stage) {
        for (std::uint32_t i = 0; i < lanes; ++i) {
            SCOPED_TRACE(testing::Message() << "stage " << stage << " lane " << i);
            const bool active = ((stage_exec[stage] >> i) & 1) != 0;
            EXPECT_EQ(out[stage * lanes + i], active ? 0x100 | i : 0u);
        }
    }
}

// VALU mask results: compares into VCC and SGPR pairs (signed and unsigned), v_cndmask_b32, the
// carries and borrows in and out, the bits of inactive lanes, which a compare clears, and v_cmpx
// into a pair.
TEST_F(TranslateExec, MaskAlu) {
    const auto out = run("mask_alu", {12, 0xffff'ffe0, 10}, 8 * lanes);
    constexpr std::uint32_t exec_lo = 0x0f0f'0f0f, exec_hi = 0x00ff'00ff;
    for (std::uint32_t i = 0; i < lanes; ++i) {
        SCOPED_TRACE(testing::Message() << "lane " << i);
        const bool active = ((i < 32 ? exec_lo >> i : exec_hi >> (i - 32)) & 1) != 0;
        EXPECT_EQ(out[i], i > 12 ? 20u : 10u);
        EXPECT_EQ(out[lanes + i], i < 32 ? 20u : 10u);
        EXPECT_EQ(out[2 * lanes + i], i >= 32 ? 20u : 10u);
        EXPECT_EQ(out[3 * lanes + i], i >= 32 ? 1u : 0u);
        EXPECT_EQ(out[4 * lanes + i], i < 10 ? ~0u : 0u);
        EXPECT_EQ(out[5 * lanes + i], active ? 20u : 10u);
        EXPECT_EQ(out[6 * lanes + i], i < 40 ? 11u : 0u);
        // Carry in c = bit 0 of the lane, a = -1 where bit 1 is set.
        const std::uint32_t c = i & 1, a = (i & 2) != 0 ? ~0u : 0u;
        const std::uint32_t bits[] = {std::uint64_t{a} + c > ~0u, a < c, std::uint64_t{a} + c > 0,
                                      i < a};
        EXPECT_EQ(out[7 * lanes + i], bits[0] << 3 | bits[1] << 2 | bits[2] << 1 | bits[3]);
    }
}

// All 16 compare conditions, s2 against i - 32, with the operands in both signednesses.
TEST_F(TranslateExec, MaskCompare) {
    constexpr std::int32_t s2 = -16;
    const auto out = run("mask_cmp", {static_cast<std::uint32_t>(s2)}, lanes);
    const auto bits = [](auto a, auto b) {
        return std::array<bool, 8>{false, a < b, a == b, a <= b, a > b, a != b, a >= b, true};
    };
    for (std::uint32_t i = 0; i < lanes; ++i) {
        SCOPED_TRACE(testing::Message() << "lane " << i);
        const std::int32_t v = static_cast<std::int32_t>(i) - 32;
        std::uint32_t expected = 0;
        for (bool bit : bits(s2, v)) {
            expected = expected << 1 | bit;
        }
        for (bool bit : bits(static_cast<std::uint32_t>(s2), static_cast<std::uint32_t>(v))) {
            expected = expected << 1 | bit;
        }
        EXPECT_EQ(out[i], expected);
    }
}

// f32 arithmetic with and without modifiers, conversions and rounding, against the same
// operations on the host (which rounds to nearest even, like the shader's float mode).
TEST_F(TranslateExec, FloatAlu) {
    constexpr std::uint32_t stages = 31;
    const auto out = run("float_alu", {}, stages * lanes);
    const auto f32 = [](std::uint32_t bits) { return std::bit_cast<float>(bits); };
    const auto u32 = [](float f) { return std::bit_cast<std::uint32_t>(f); };
    for (std::uint32_t i = 0; i < lanes; ++i) {
        const float x = static_cast<float>(i) - 32, y = 0.75f * x;
        const float inf = std::numeric_limits<float>::infinity();
        const float prod = x * y, third = 0.75f;
        const std::uint32_t expected[stages] = {
            u32(x + third), u32(third - x), u32(x - third), u32(prod + x),
            u32(std::fma(x, y, 1.0f)),
            u32(std::max(third, x)), u32(std::min(0.0f, x)), u32(-std::fabs(x) * y),
            u32(std::clamp(prod, 0.0f, 1.0f)), u32(std::clamp(4 * prod, 0.0f, 1.0f)),
            u32(prod / 2), static_cast<std::uint32_t>(static_cast<std::int32_t>(y)),
            y <= 0 ? 0u : static_cast<std::uint32_t>(y),
            x < 0 ? 0x8000'0000u : (x > 0 ? 0x7fff'ffffu : 0u), x != 0 ? ~0u : 0u,
            static_cast<std::uint32_t>(static_cast<std::int32_t>(std::floor(y))),
            static_cast<std::uint32_t>(static_cast<std::int32_t>(std::floor(y + 0.5f))),
            u32(y - std::floor(y)), u32(std::floor(y)), u32(std::ceil(y)), u32(std::trunc(y)),
            u32(std::nearbyint(y)), u32(i == 0 ? 0.0f : 1.0f), u32(x == 0 ? 0.0f : inf * x),
            u32(inf * x), u32(2.0f + 0.75f * x), u32(x * 0.75f + y), u32(prod + third),
            u32(third < x ? third : x), u32(third >= x ? third : x),
            u32(std::nextafter(1.0f, 0.0f)),  // fract(-2^-26) stays below 1
        };
        for (std::uint32_t stage = 0; stage < stages; ++stage) {
            if (stage == 24 && x == 0) {  // infinity * 0 is a NaN, whatever its sign
                EXPECT_TRUE(std::isnan(f32(out[stage * lanes + i]))) << "lane " << i;
            } else {
                EXPECT_EQ(out[stage * lanes + i], expected[stage])
                    << "stage " << stage << ", lane " << i << ", x " << x;
            }
        }
    }
}

// NaN operands, infinities and saturation, the legacy multiply, a mad that must not be fused
// next to an fma that is, and clamp and rounding under three float modes: the default (round to
// nearest even), DX10_CLAMP alone, and round toward zero alone. The last one runs only when the
// device reports shaderRoundingModeRTZFloat32, since the module would be invalid otherwise.
TEST_F(TranslateExec, FloatEdgeCases) {
    constexpr std::uint32_t stages = 13, dx10_clamp = 1u << 21, round_to_zero = 3u << 12;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    const auto f32 = [](std::uint32_t bits) { return std::bit_cast<float>(bits); };
    const auto u32 = [](float f) { return std::bit_cast<std::uint32_t>(f); };
    std::vector<std::uint32_t> modes = {0u, dx10_clamp};
    if (runner_->info().round_rtz_f32) {
        modes.push_back(round_to_zero);
    }
    for (const std::uint32_t mode : modes) {
        const bool dx10 = (mode & dx10_clamp) != 0;
        const bool zero = (mode & round_to_zero) != 0;
        const auto out = run("float_edge", {}, stages * lanes, mode);
        for (std::uint32_t i = 0; i < lanes; ++i) {
            SCOPED_TRACE(testing::Message() << "mode " << mode << ", lane " << i);
            const float x = static_cast<float>(i) - 32;
            const float a = i % 2 == 1 ? nan : x, b = i / 2 % 2 == 1 ? nan : 2.5f;
            const float cs[] = {inf, -inf, nan, 3e9f};
            const float c = cs[i % 4];  // the program's c
            const auto at = [&](std::uint32_t stage) { return out[stage * lanes + i]; };
            const auto check_float = [&](std::uint32_t stage, float expected) {  // any NaN
                if (std::isnan(expected)) {
                    EXPECT_TRUE(std::isnan(f32(at(stage)))) << "stage " << stage;
                } else {
                    EXPECT_EQ(at(stage), u32(expected)) << "stage " << stage;
                }
            };
            check_float(0, std::isnan(a) ? b : (std::isnan(b) ? a : std::min(a, b)));
            check_float(1, std::isnan(a) ? b : (std::isnan(b) ? a : std::max(a, b)));
            check_float(2, std::isnan(a) ? (dx10 ? 0.0f : nan) : std::clamp(a, 0.0f, 1.0f));
            EXPECT_EQ(at(3), std::isnan(c) ? 0u : (c < 0 ? 0x8000'0000u : 0x7fff'ffffu));
            EXPECT_EQ(at(4), std::isnan(c) || c < 0 ? 0u : (c < inf ? 3'000'000'000u : ~0u));
            check_float(5, 1.0f);
            check_float(6, i % 4 == 3 ? 1.0f : nan);  // 0 * inf and 0 * NaN
            check_float(7, 2.0f);
            EXPECT_EQ(at(8), 0u);
            EXPECT_EQ(at(9), u32(0x1p-24f));
            EXPECT_EQ(at(10), 0u);
            EXPECT_EQ(at(11), zero ? 0x3f80'0000u : 0x3f80'0001u);
            EXPECT_EQ(at(12), zero ? 0x4c00'0000u : 0x4c00'0001u);
        }
    }
}

// All 16 float compare conditions with NaN operands, the VOP3 form with modifiers, and v_cmpx.
TEST_F(TranslateExec, FloatCompare) {
    const auto out = run("float_cmp", {}, 3 * lanes);
    for (std::uint32_t i = 0; i < lanes; ++i) {
        SCOPED_TRACE(testing::Message() << "lane " << i);
        const float a = 0.0f;
        const float b = i % 8 == 7 ? std::numeric_limits<float>::quiet_NaN()
                                   : static_cast<float>(i) - 32;
        const bool conditions[16] = {
            false, a < b, a == b, a <= b, a > b, a < b || a > b, a >= b, a == a && b == b,
            a != a || b != b, !(a >= b), !(a < b || a > b), !(a > b), !(a <= b), !(a == b),
            !(a < b), true,
        };
        std::uint32_t expected = 0;
        for (const bool bit : conditions) {
            expected = expected << 1 | bit;
        }
        EXPECT_EQ(out[i], expected);
        EXPECT_EQ(out[lanes + i], -std::fabs(b) < -1.0f ? 1u : 0u);
        EXPECT_EQ(out[2 * lanes + i], b < 0 ? 5u : 0u);
    }
}

// Cross-lane operations in lane-local mode: a lane sees the bits of its own subgroup at their wave
// positions, and those of the other subgroups as 0. The subgroup size is what stage 1 counts; it
// must be a power of two in the device's range. Stage 0 checks that each subgroup covers
// consecutive lanes from a multiple of its size (subgroup_base is uniform).
TEST_F(TranslateExec, CrossLane) {
    constexpr std::uint32_t m_lo = 0x8000'00d2, m_hi = 0xf0f0'f0f0, select = 0x45;
    constexpr std::uint64_t m = std::uint64_t{m_hi} << 32 | m_lo;
    const auto out = run("cross_lane", {m_lo, m_hi, select}, 14 * lanes);
    const std::uint32_t size = out[lanes];
    const DeviceInfo& info = runner_->info();
    ASSERT_TRUE(std::has_single_bit(size) && size >= info.min_subgroup_size &&
                size <= std::min(info.max_subgroup_size, lanes))
        << "subgroup size " << size;
    const auto ff1 = [](std::uint64_t x) {
        return x == 0 ? ~0u : static_cast<std::uint32_t>(std::countr_zero(x));
    };
    const auto count = [](std::uint64_t x) { return static_cast<std::uint32_t>(std::popcount(x)); };
    const std::uint32_t lane = select & 63;
    for (std::uint32_t i = 0; i < lanes; ++i) {
        SCOPED_TRACE(testing::Message() << "lane " << i << ", subgroup size " << size);
        const std::uint32_t base = i & ~(size - 1);
        const std::uint64_t window = (size == 64 ? ~0ull : (1ull << size) - 1) << base;
        const std::uint64_t w = m & window;
        const std::uint32_t below_20 = count(0xf'ffff & window);
        const std::uint32_t expected[14] = {
            base,
            size,
            i,
            count(w & ((1ull << i) - 1)),
            64 - count(w),
            ff1(w),
            ff1(~w),
            w == 0 ? ~0u : static_cast<std::uint32_t>(std::countl_zero(w)),
            ff1(w >> 32),
            below_20 != 0 ? below_20 : 0xdead,
            57 << 8 | 4,  // s_flbit_i32 of 0xf0f0f0f0, s_flbit_i32_i64 of 0x45
            lane - base < size ? 0x100 | lane : 0,
            w != 0 ? 0x100 | ff1(w) : (base == 0 ? 0x100 : 0),  // lane 0 when none is active
            i == lane ? 7 : 0x100 | i,
        };
        for (std::uint32_t stage = 0; stage < 14; ++stage) {
            EXPECT_EQ(out[stage * lanes + i], expected[stage]) << "stage " << stage;
        }
    }
}

// ds_swizzle_b32 in lane-local mode: the source lane is worked out from the pattern as in the ISA
// (a quad permutation, or ((i & and) | or) ^ xor within the 32 lanes), and it reads 0 when that
// lane is inactive or in another subgroup. Lane i holds i + 1. The last two stages run with the
// lane 3 of each quad inactive: that lane keeps its old value and is read as 0.
TEST_F(TranslateExec, DsSwizzle) {
    // The pattern of stages 1 to 11 (the program's offsets); stages 10 and 11 run masked.
    constexpr std::uint32_t patterns[11] = {0x801b, 0x80aa, 0x8071, 0x041f, 0x401f, 0x7c1f,
                                            0x0000, 0x0103, 0x0e0f, 0x801b, 0x041f};
    const auto out = run("ds_swizzle", {}, 12 * lanes);
    const std::uint32_t size = out[0];
    const DeviceInfo& info = runner_->info();
    ASSERT_TRUE(std::has_single_bit(size) && size >= info.min_subgroup_size &&
                size <= std::min(info.max_subgroup_size, lanes))
        << "subgroup size " << size;
    const auto source = [](std::uint32_t pattern, std::uint32_t i) {
        if (pattern & 0x8000) {
            return (i & ~3u) | (pattern >> (2 * (i & 3)) & 3);
        }
        const std::uint32_t local = (((i & 31) & (pattern & 31)) | (pattern >> 5 & 31)) ^
                                    (pattern >> 10 & 31);
        return (i & 32) | local;
    };
    // Hand-worked: with 0x801b lane 5 (position 1 of the quad 4 to 7) reads position 2, lane 6.
    EXPECT_EQ(source(0x801b, 5), 6u);
    EXPECT_EQ(source(0x401f, 37), 37u ^ 16);
    EXPECT_EQ(source(0x0103, 45), 32u + 8 + 1);
    for (std::uint32_t stage = 1; stage <= 11; ++stage) {
        const std::uint32_t pattern = patterns[stage - 1];
        const bool masked = stage >= 10;
        for (std::uint32_t i = 0; i < lanes; ++i) {
            SCOPED_TRACE(testing::Message() << "stage " << stage << ", pattern " << std::hex
                                            << pattern << ", lane " << std::dec << i
                                            << ", subgroup size " << size);
            const std::uint32_t j = source(pattern, i);
            const bool readable = j / size == i / size && !(masked && (j & 3) == 3);
            const std::uint32_t expected = masked && (i & 3) == 3 ? 0xdead : readable ? j + 1 : 0;
            EXPECT_EQ(out[stage * lanes + i], expected);
        }
    }
}

// The user data of the memory programs: the output, the table through a pointer, the V# of a
// buffer (stride, number of records) at the table, the V# of the buffer to store to, and soffset.
std::vector<std::uint32_t> memory_user_data(std::uint32_t stride, std::uint32_t records,
                                            std::uint32_t store_records = 0,
                                            std::uint32_t soffset = 0) {
    const std::uint64_t table = base + data_offset, store = base + store_offset;
    return {static_cast<std::uint32_t>(table),
            static_cast<std::uint32_t>(table >> 32),
            static_cast<std::uint32_t>(table),
            static_cast<std::uint32_t>(table >> 32) | stride << 16,
            records,
            0,
            static_cast<std::uint32_t>(store),
            static_cast<std::uint32_t>(store >> 32),
            store_records,
            0,
            soffset};
}

// s_load_* and s_buffer_load_*, run with EXEC = 0. The V# covers 24 bytes, as 24 raw bytes or as 3
// records of 8 (the size is stride * num_records, 7.2.2 and Table 8.5), 22 raw bytes (a dword at
// byte 20 is inside, one at 24 is not), or 4 KiB. s_buffer_load_* reads 0 for each dword that is
// not inside it. The dword at byte 24 is outside in both 24-byte cases: the clamp is `>=` for a
// stride too, where the manual's `>` would let it through.
TEST_F(TranslateExec, ScalarMemory) {
    // The table dword that each value is read from: the first 8 by s_load_*, the others by
    // s_buffer_load_*. The offset of value 7 and of the last two is in an SGPR, in bytes: 36, 36
    // and 19 (the low 2 bits are ignored).
    constexpr std::uint32_t index[18] = {1, 2, 3, 4, 5, 6, 7, 9, 1, 5, 6, 4, 5, 6, 7, 0x40, 9, 4};
    for (const auto [stride, records] : {std::pair{0u, 24u}, {0u, 22u}, {8u, 3u}, {0u, 0x1000u}}) {
        SCOPED_TRACE(testing::Message() << "stride " << stride << " records " << records);
        const auto out = run("mem_smrd", memory_user_data(stride, records), 18 * lanes);
        for (std::uint32_t k = 0; k < 18; ++k) {
            const bool inside = k < 8 || records == 0x1000 || index[k] < 6;
            for (std::uint32_t i = 0; i < lanes; ++i) {
                ASSERT_EQ(out[k * lanes + i], inside ? data_word(index[k]) : 0u)
                    << "value " << k << " lane " << i;
            }
        }
    }
}

// Untyped MUBUF dword loads and stores: offen, idxen, both, a dword pair, addr64, soffset, and
// lanes that are inactive or out of range. The range rule is Sea Islands ISA 8.1.5.3, checked per
// dword. With a stride of 0, offset + soffset >= num_records (bytes) is out of range, whatever the
// index, and a soffset beyond the size is always out (the subtraction of the manual does not wrap).
// With a stride, an access is out of range when index >= num_records, or with idxen when the
// offset is not below the stride; without idxen the index is 0 and the offset is not checked.
// Loads outside read 0, stores outside are dropped, addr64 is not checked.
TEST_F(TranslateExec, BufferMemory) {
    constexpr std::uint32_t store_records = 0x22a;  // 554 bytes, not a multiple of 4
    constexpr std::uint32_t sentinel = 0x77, far_offset = 0x44;  // offset:0x40 plus a VGPR of 4
    struct Config {
        std::uint32_t stride, records, soffset;
    };
    // The raw size 102 cuts the pair of lane 25 after its first dword (100 < 102, 104 >= 102). In
    // the raw size 104 lane 20 at soffset 16 is exactly at the size (24 + 80). A table of 0
    // records and a stride is out for everything. The soffset 0x1000 is beyond the size of 8.
    for (const auto [stride, records, soffset] :
         {Config{0, 102, 16}, {0, 104, 16}, {0, 8, 0x1000}, {16, 20, 16}, {16, 0, 16}}) {
        SCOPED_TRACE(testing::Message() << "stride " << stride << " records " << records
                                        << " soffset " << soffset);
        const auto inside = [&](std::uint32_t index, bool idxen, std::uint32_t offset,
                                std::uint32_t dword, std::uint32_t soff) {
            offset += 4 * dword;
            return stride == 0 ? std::uint64_t{offset} + soff < records
                               : index < records && (!idxen || offset < stride);
        };
        const auto data = [](bool ok, std::uint32_t address) {
            return ok ? data_word(address / 4) : 0u;
        };
        const auto out = run("mem_mubuf", memory_user_data(stride, records, store_records, soffset),
                             0x800);
        const auto slot = [&](std::uint32_t k, std::uint32_t i) { return out[k * lanes + i]; };
        for (std::uint32_t i = 0; i < lanes; ++i) {
            SCOPED_TRACE(testing::Message() << "lane " << i);
            const std::uint32_t at = i * stride;
            // offen with soffset: part of the check with a stride of 0, otherwise only of the
            // address
            EXPECT_EQ(slot(0, i),
                      data(inside(0, false, 4 * i + 8, 0, soffset), soffset + 4 * i + 8));
            // idxen: the index against num_records
            EXPECT_EQ(slot(1, i), data(inside(i, true, 0, 0, 0), at));
            // idxen and offen with an offset beyond the stride
            EXPECT_EQ(slot(2, i), data(inside(i, true, far_offset, 0, 0), at + far_offset));
            const bool exec = i < 32 && (i & 7) < 4;  // exec_lo = 0x0f0f0f0f, exec_hi = 0
            EXPECT_EQ(slot(3, i), exec ? data(inside(0, false, 4 * i, 0, 0), 4 * i) : sentinel);
            // a pair with offen and no idxen, a dword at a time: no offset check with a stride
            EXPECT_EQ(slot(4, i), data(inside(0, false, 4 * i, 0, 0), 4 * i));
            EXPECT_EQ(slot(5, i), data(inside(0, false, 4 * i, 1, 0), 4 * i + 4));
            // addr64: never checked, soffset is added to the address. The table ends before the
            // soffset 0x1000, where the memory is zero.
            EXPECT_EQ(slot(6, i), soffset < 0x1000 ? data_word(i + soffset / 4) : 0u);
            // the offset against the stride 16: 12 is inside, 16 is not, and the pair from 12 is
            // cut between its dwords. Slot 7 has a soffset: with a stride it is only part of the
            // address, so the offset 12 stays inside the stride 16 whatever the soffset.
            EXPECT_EQ(slot(7, i), data(inside(i, true, 12, 0, soffset), at + 12 + soffset));
            EXPECT_EQ(slot(8, i), data(inside(i, true, 16, 0, 0), at + 16));
            EXPECT_EQ(slot(9, i), data(inside(i, true, 12, 0, 0), at + 12));
            EXPECT_EQ(slot(10, i), data(inside(i, true, 12, 1, 0), at + 16));
        }
        // The stores: one dword per lane, and a pair per lane at 0x200 + 8 * lane. The size cuts
        // the pair of lane 5 (bytes 552 and 556) after its first dword, and that dword ends past
        // the size (552 + 4 > 554), so only a per-dword start check keeps it.
        const auto stored = [&](std::uint32_t byte) { return out[(store_offset + byte) / 4]; };
        for (std::uint32_t i = 0; i < lanes; ++i) {
            EXPECT_EQ(stored(4 * i), 0xa000 | i) << "lane " << i;
            const std::uint32_t pos = 0x200 + 8 * i;
            EXPECT_EQ(stored(pos), pos < store_records ? (0xa000 | i) : 0u) << "lane " << i;
            EXPECT_EQ(stored(pos + 4), pos + 4 < store_records ? (0xb000 | i) : 0u)
                << "lane " << i;
        }
    }
}

// SALU lane masks: s_and_saveexec_b64 with v_cmpx (nested predication), the eight bitwise
// operations on two masks with every combination of bits in lanes 0 to 3, and the same
// operations under a partial EXEC (lanes 0-31), whose results have bits in every lane. The
// saveexec forms run with S0 and EXEC that differ in both directions: S0 is the odd lanes, so
// s_andn2_saveexec gives S0 & ~EXEC (the odd lanes from 32 on) and not EXEC & ~S0.
TEST_F(TranslateExec, MaskScalarOps) {
    const auto out = run("mask_exec", {}, 4 * lanes);
    for (std::uint32_t i = 0; i < lanes; ++i) {
        SCOPED_TRACE(testing::Message() << "lane " << i);
        EXPECT_EQ(out[i], (i < 4 ? 0x20u : 0u) | (i < 8 ? 0x12u : i < 32 ? 0x11u : 0u));
        std::uint32_t bits = 0;
        for (unsigned k = 0; k < 8; ++k) {
            bits = bits << 1 | (bitwise(k, i & 1, i >> 1 & 1) & 1);
        }
        EXPECT_EQ(out[lanes + i], bits);
        const std::uint32_t a = i & 1, b = i >> 1 & 1, in_exec = i < 32;
        EXPECT_EQ(out[2 * lanes + i], (bitwise(1, a, b) & 1) << 1 | (bitwise(6, a, b) & 1));
        EXPECT_EQ(out[3 * lanes + i], (bitwise(3, a, in_exec) & 1) << 2 | in_exec << 1 |
                                          (bitwise(4, a, in_exec) & 1));
    }
}

// SCC after a mask operation is any of the result, and after a value operation result != 0. The
// data is the same in every lane, so the branches don't depend on the subgroup size. The
// chain of the eight operations on values is checked against the same operations here.
TEST_F(TranslateExec, SccAndBitwiseValues) {
    constexpr std::uint32_t a = 0x1234'5678, other = 0x0f0f'ff00, c = 0x9abc'def0;
    for (const std::uint32_t b : {a, other}) {
        SCOPED_TRACE(b);
        const auto out = run("mask_scc", {a, b, c}, 3 * lanes);
        std::uint32_t chain = a & b;
        const std::uint32_t srcs[] = {0, c, a, b, c, a, b, c};
        for (unsigned k = 1; k < 8; ++k) {
            chain = bitwise(k, chain, srcs[k]);
        }
        for (std::uint32_t i = 0; i < lanes; ++i) {
            EXPECT_EQ(out[i], b == a ? 1u + 2 + 4 + 16 : 2u + 4 + 8 + 32);
            EXPECT_EQ(out[lanes + i], 3u);
            EXPECT_EQ(out[2 * lanes + i], chain);
        }
    }
}

// An EXEC loop that each lane leaves after i & 7 rounds (s_cbranch_execz), and the branches on VCC,
// SCC (after s_cmp) and EXEC. Lane 3 of every 8 has the bit, so that every subgroup of 8 or more
// lanes takes the same branches and the results don't depend on the subgroup size.
TEST_F(TranslateExec, MaskBranches) {
    const auto out = run("mask_branch", {0xffff'fffe, 1}, 2 * lanes);
    for (std::uint32_t i = 0; i < lanes; ++i) {
        EXPECT_EQ(out[i], i & 7) << "lane " << i;
        EXPECT_EQ(out[lanes + i], 1u + 4 + 16 + 32) << "lane " << i;
    }
}

// The 12 s_cmp opcodes, one SCC bit each: s2 = -2 against s3 = 1 (signed less, unsigned greater)
// and s3 against itself. The expected bits come from the same comparisons in C++.
TEST_F(TranslateExec, ScalarCompares) {
    constexpr std::uint32_t s2 = 0xffff'fffe, s3 = 1;
    const auto bits = [](auto a, auto b) {
        // eq, lg, gt, ge, lt, le
        return std::array<bool, 6>{a == b, a != b, a > b, a >= b, a < b, a <= b};
    };
    const auto mask = [&](std::uint32_t a, std::uint32_t b) {
        std::uint32_t result = 0;
        unsigned n = 0;
        for (bool bit : bits(static_cast<std::int32_t>(a), static_cast<std::int32_t>(b))) {
            result |= std::uint32_t{bit} << n++;
        }
        for (bool bit : bits(a, b)) {
            result |= std::uint32_t{bit} << n++;
        }
        return result;
    };
    const auto out = run("mask_sopc", {s2, s3}, 2 * lanes);
    for (std::uint32_t i = 0; i < lanes; ++i) {
        EXPECT_EQ(out[i], mask(s2, s3)) << "lane " << i;
        EXPECT_EQ(out[lanes + i], mask(s3, s3)) << "lane " << i;
    }
}

// LDS reads and writes in two waves: dwords, the pair and st64 forms of 32 and 64 bits, 96-bit
// accesses, and the barrier that makes the other wave's writes visible. Stage k is out[128k + i].
TEST_F(TranslateExec, LdsReadWrite) {
    constexpr std::uint32_t n = 128;
    const auto out = run("lds_rw", {}, 19 * n, 0, n);
    for (std::uint32_t i = 0; i < n; ++i) {
        SCOPED_TRACE(testing::Message() << "lane " << i);
        EXPECT_EQ(out[i], 5 * (n - 1 - i) + 7);
        EXPECT_EQ(out[n + i], 5 * i + 7);
        EXPECT_EQ(out[2 * n + i], i);
        EXPECT_EQ(out[3 * n + i], i);  // read2 with offset0:1 reads the second dword first
        EXPECT_EQ(out[4 * n + i], 5 * i + 7);
        EXPECT_EQ(out[5 * n + i], 5 * i + 7);
        EXPECT_EQ(out[6 * n + i], i);
        // write2st64_b64 with offsets 0 and 2 (512 bytes each) read back by read2_b64 with offsets
        // 0 and 128 (8 bytes each), then write2_b64 with the pairs swapped read by read2st64_b64
        const std::uint32_t first[4] = {5 * i + 7, i, 5 * i + 0x107, 2 * i};
        const std::uint32_t swapped[4] = {5 * i + 0x107, 2 * i, 5 * i + 7, i};
        for (std::uint32_t k = 0; k < 4; ++k) {
            EXPECT_EQ(out[(7 + k) * n + i], first[k]) << "read2_b64 dword " << k;
            EXPECT_EQ(out[(11 + k) * n + i], swapped[k]) << "read2st64_b64 dword " << k;
        }
        // write_b96 and write_b64 at 16i (the third dword is replaced), read_b96 at 16i, and the
        // fourth dword by read_b32
        EXPECT_EQ(out[15 * n + i], 5 * i + 7);
        EXPECT_EQ(out[16 * n + i], i);
        EXPECT_EQ(out[17 * n + i], i + 0x2000);
        EXPECT_EQ(out[18 * n + i], i + 0x3000);
    }
}

// Lanes of one wave exchange data through the LDS with no barrier: a read sees the neighbour's
// earlier write and not its later one. Lavapipe's subgroups are 8 lanes, and the pair is inside
// one.
TEST_F(TranslateExec, LdsBetweenLanesOfAWave) {
    const auto out = run("lds_wave", {}, 2 * lanes);
    for (std::uint32_t i = 0; i < lanes; ++i) {
        EXPECT_EQ(out[i], (i ^ 1) + 7) << "lane " << i;
        EXPECT_EQ(out[lanes + i], i + 107) << "lane " << i;
    }
}

// Bytes, halfwords and 64 to 128 bits. Neighbouring lanes write bytes of one dword at the same
// time, and many lanes write the same byte or halfword with a bit of their own: one of them must
// win whole, not all of them or-ed together. Accesses at and past the end of the LDS read 0 and
// write nothing.
TEST_F(TranslateExec, LdsNarrowWideAndOutOfRange) {
    constexpr std::uint32_t n = 128;
    const auto out = run("lds_narrow", {}, 12 * n, 0, n);
    const auto byte = [](std::uint32_t i) { return ((i + 1) | 0x80) & 0xff; };
    for (std::uint32_t i = 0; i < n; ++i) {
        SCOPED_TRACE(testing::Message() << "lane " << i);
        EXPECT_EQ(out[i], byte(i));
        EXPECT_EQ(out[n + i], 0xffff'ff00 | byte(i));
        EXPECT_EQ(out[2 * n + i], byte(i) << 8 | byte(i));
        EXPECT_EQ(out[3 * n + i], 0xffff'0000 | byte(i) << 8 | byte(i));
        EXPECT_EQ(out[4 * n + i], byte(i & ~1u) | byte(i | 1) << 8);
        EXPECT_EQ(out[5 * n + i], i + 1);
        EXPECT_EQ(out[6 * n + i], i + 3);
        EXPECT_EQ(out[7 * n + i], i + 2);
        EXPECT_EQ(out[8 * n + i], 0u);
        EXPECT_EQ(out[9 * n + i], 0x5a5a'5a5au);
        // Lane j writes the bit j % 8 to the byte j % 4 and the bit j % 16 to the halfword j % 2.
        // The lanes of one address agree on the winner, and it is the bit of one of them.
        const auto won = [&](std::uint32_t index, std::uint32_t slot, std::uint32_t slots) {
            const std::uint32_t v = out[index * n + i];
            EXPECT_EQ(v, out[index * n + slot]) << "lanes disagree";
            EXPECT_TRUE(std::has_single_bit(v) && std::countr_zero(v) % slots == slot)
                << "not one lane's value: " << v;
        };
        won(10, i % 4, 4);
        won(11, i % 2, 2);
    }
}

// The LDS atomics: every lane applies the same operation to a dword, or its own operand. The
// result of the ones that don't commute doesn't depend on the order, because the lanes' operands
// are equal. The exchange returns are checked as a whole: what the lanes got plus what is left
// is what was put in.
TEST_F(TranslateExec, LdsAtomics) {
    constexpr std::uint32_t n = 128;
    const auto out = run("lds_atomic", {}, 4 * n, 0, n);
    std::uint32_t xor_all = 0, inc = 8, dec = 2, rsub = 100;
    for (std::uint32_t i = 0; i < n; ++i) {
        xor_all ^= i + 1;
        inc = inc >= 10 ? 0 : inc + 1;
        dec = dec == 0 || dec > 5 ? 5 : dec - 1;
        rsub = 30 - rsub;
    }
    rsub = 30 - rsub;  // the extra one from lane 0 (an even count of them would undo itself)
    const std::uint32_t expected[16] = {
        n * (n + 1) / 2, 0u - n * (n + 1) / 2, ~0u, xor_all, 0, 5, ~0u, 0u - 64, 63,
        out[9], inc, dec, rsub, (0xf0f0'f0f0 & ~0x00ff'00ffu) | 0x1200'3400, 9, n};
    for (std::uint32_t i = 0; i < 16; ++i) {
        EXPECT_EQ(out[i], expected[i]) << "dword " << i;
    }
    EXPECT_GE(out[9], 1u);
    EXPECT_LE(out[9], n);
    std::uint32_t exchanged = out[9], swapped = 0;
    std::vector<std::uint32_t> added;
    for (std::uint32_t i = 0; i < n; ++i) {
        exchanged += out[n + i];
        swapped += out[2 * n + i] == 5;
        EXPECT_EQ(out[2 * n + i] == 5 || out[2 * n + i] == 9, true) << "lane " << i;
        added.push_back(out[3 * n + i]);
    }
    EXPECT_EQ(exchanged, n * (n + 1) / 2);
    EXPECT_EQ(swapped, 1u);  // the first lane swaps 5 for 9, the others find 9
    std::ranges::sort(added);
    for (std::uint32_t i = 0; i < n; ++i) {
        EXPECT_EQ(added[i], i);
    }
}

TEST_F(TranslateExec, ScalarLoopAndSkips) {
    const auto out = run("scalar_loop", {5, 0x8000'0001}, lanes + 1);
    for (std::uint32_t i = 0; i < lanes; ++i) {
        EXPECT_EQ(out[i], (5u + 4 + 3 + 2 + 1 + 64) ^ i) << "lane " << i;
    }
    EXPECT_EQ(out[lanes], 0u);
}

}  // namespace
