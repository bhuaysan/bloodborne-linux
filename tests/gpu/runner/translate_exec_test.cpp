// Execution tests of the translator on lavapipe: our GCN programs in tests/gpu/programs/, run as
// one workgroup of 64 lanes, against values worked out from the ISA manual. The modules are the
// ones that GcnTranslate.ProgramsPassSpirvVal validates (same programs, same key).

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
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

    // Runs the program with user_data = {output address, then `inputs`} and returns the first
    // `words` dwords of the guest memory, 0x3000 bytes with the table of the memory programs
    // (data_word) at data_offset.
    std::vector<std::uint32_t> run(std::string_view name, std::vector<std::uint32_t> inputs,
                                   std::uint32_t words) {
        const auto translation = bb::gcn::translate(
            bb::test::gcn::program_words(name), {},
            {.pgm_rsrc2 = bb::test::gcn::program_user_sgprs(name) << 1});
        auto memory = runner_->guest_memory(base, 0x3000);
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
// records of 8 (unverified), 22 raw bytes (a dword at byte 20 is inside, one at 24 is not), or
// 4 KiB. s_buffer_load_* reads 0 for each dword that is not inside it.
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
// lanes that are inactive or out of range. The range rule (docs/design/graphics.md): each dword
// is checked on its own. A raw access (no idxen, or stride 0) is inside when offset + 4 * d <
// size, with size = num_records bytes (stride 0) or num_records * stride (unverified); a
// structured one (idxen and a stride) when index < num_records, whatever its offset. Loads outside
// read 0, stores outside are dropped, addr64 is not checked, and soffset never counts.
TEST_F(TranslateExec, BufferMemory) {
    constexpr std::uint32_t soffset = 16, store_records = 0x22a;  // 554 bytes, not a multiple of 4
    constexpr std::uint32_t sentinel = 0x77, far_offset = 0x44;   // offset:0x40 plus a VGPR of 4
    // The raw size 102 cuts the pair of lane 25 after its first dword (100 < 102, 104 >= 102).
    for (const auto [stride, records] : {std::pair{0u, 102u}, {16u, 20u}}) {
        SCOPED_TRACE(testing::Message() << "stride " << stride << " records " << records);
        const std::uint32_t size = records * std::max(stride, 1u);
        const auto raw = [&](std::uint32_t offset, std::uint32_t dword = 0) {
            return offset + 4 * dword < size;
        };
        const auto structured = [&](std::uint32_t index, std::uint32_t offset) {
            return stride != 0 ? index < records : raw(offset);
        };
        const auto data = [](bool ok, std::uint32_t address) {
            return ok ? data_word(address / 4) : 0u;
        };
        const auto out = run("mem_mubuf", memory_user_data(stride, records, store_records, soffset),
                             0x800);
        const auto slot = [&](std::uint32_t k, std::uint32_t i) { return out[k * lanes + i]; };
        for (std::uint32_t i = 0; i < lanes; ++i) {
            SCOPED_TRACE(testing::Message() << "lane " << i);
            // offen: soffset moves the address, not the check
            EXPECT_EQ(slot(0, i), data(raw(4 * i + 8), soffset + 4 * i + 8));
            // idxen: index against num_records
            EXPECT_EQ(slot(1, i), data(structured(i, 0), i * stride));
            // idxen and offen with an offset beyond the stride: not part of the check
            EXPECT_EQ(slot(2, i), data(structured(i, far_offset), i * stride + far_offset));
            const bool exec = i < 32 && (i & 7) < 4;  // exec_lo = 0x0f0f0f0f, exec_hi = 0
            EXPECT_EQ(slot(3, i), exec ? data(raw(4 * i), 4 * i) : sentinel);
            EXPECT_EQ(slot(4, i), data(raw(4 * i), 4 * i));  // pair, a dword at a time
            EXPECT_EQ(slot(5, i), data(raw(4 * i, 1), 4 * i + 4));
            EXPECT_EQ(slot(6, i), data_word(i));  // addr64: never checked
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

TEST_F(TranslateExec, ScalarLoopAndSkips) {
    const auto out = run("scalar_loop", {5, 0x8000'0001}, lanes + 1);
    for (std::uint32_t i = 0; i < lanes; ++i) {
        EXPECT_EQ(out[i], (5u + 4 + 3 + 2 + 1 + 64) ^ i) << "lane " << i;
    }
    EXPECT_EQ(out[lanes], 0u);
}

}  // namespace
