// Execution tests of the typed buffer loads (MTBUF) on lavapipe: the program mem_typed loads 15
// formats from a table, and each result is worked out here from the element's bytes with the
// usual conversions of normalized, scaled and integer formats (docs/design/graphics.md, "Typed
// buffer formats").

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "gpu/gcn/gcn.hpp"
#include "gpu/gcn/programs.hpp"
#include "gpu/gcn/translate.hpp"
#include "gpu/runner/compute_runner.hpp"

namespace {

using namespace bb::test::vk;
namespace nf {
constexpr unsigned unorm = 0, snorm = 1, sscaled = 3, uint_ = 4, sint = 5, snorm_ogl = 6;
}  // the number formats the program uses

constexpr std::uint64_t base = 0x12'0000'0000;  // guest address of the output
constexpr std::uint32_t table_offset = 0x8000, table_size = 2048, lanes = 64;
constexpr std::uint32_t output_size = 0x4000;  // one block of 1 KB per format
constexpr std::uint32_t identity = 4 | 5 << 3 | 6 << 6 | 7 << 9;  // dst_sel X, Y, Z, W

struct Field {
    unsigned offset, width;  // in bits
};
// The components of each data format by their bit fields, from the element's first bit.
std::vector<Field> fields(unsigned dfmt) {
    switch (dfmt) {
    case 1: return {{0, 8}};
    case 2: return {{0, 16}};
    case 3: return {{0, 8}, {8, 8}};
    case 5: return {{0, 16}, {16, 16}};
    case 9: return {{0, 10}, {10, 10}, {20, 10}, {30, 2}};
    case 10: return {{0, 8}, {8, 8}, {16, 8}, {24, 8}};
    case 11: return {{0, 32}, {32, 32}};
    case 12: return {{0, 16}, {16, 16}, {32, 16}, {48, 16}};
    case 13: return {{0, 32}, {32, 32}, {64, 32}};
    case 14: return {{0, 32}, {32, 32}, {64, 32}, {96, 32}};
    default: ADD_FAILURE() << "format " << dfmt; return {};
    }
}

// The half float `h` as an f32. A NaN becomes the quiet NaN of its sign (the table plants no other).
float half_to_float(std::uint32_t h) {
    const int e = (h >> 10) & 31;
    const float m = static_cast<float>(h & 1023);
    float v;
    if (e == 31) {
        v = m == 0 ? std::numeric_limits<float>::infinity()
                   : std::numeric_limits<float>::quiet_NaN();
    } else {
        v = e == 0 ? std::ldexp(m, -24) : std::ldexp(1024 + m, e - 25);
    }
    return (h & 0x8000) != 0 ? -v : v;
}

// One component of `bytes` (the element, little endian) in the number format, as 32 bits.
std::uint32_t component(const std::vector<std::uint8_t>& bytes, Field f, unsigned nfmt) {
    std::uint64_t u = 0;
    for (unsigned i = 0; i < f.width; ++i) {
        u |= std::uint64_t{(bytes[(f.offset + i) / 8] >> ((f.offset + i) % 8)) & 1u} << i;
    }
    const bool negative = (u >> (f.width - 1)) != 0;
    const std::int64_t s = std::int64_t(u) - (negative ? std::int64_t{1} << f.width : 0);
    const float max = static_cast<float>((std::uint64_t{1} << f.width) - 1);
    const float smax = static_cast<float>((std::uint64_t{1} << (f.width - 1)) - 1);
    switch (nfmt) {
    case nf::unorm: return std::bit_cast<std::uint32_t>(static_cast<float>(u) / max);
    case nf::snorm:
        return std::bit_cast<std::uint32_t>(std::max(static_cast<float>(s) / smax, -1.0f));
    case nf::sscaled: return std::bit_cast<std::uint32_t>(static_cast<float>(s));
    case nf::uint_: return static_cast<std::uint32_t>(u);
    case nf::sint: return static_cast<std::uint32_t>(s);
    case nf::snorm_ogl:
        return std::bit_cast<std::uint32_t>(static_cast<float>(2 * s + 1) / max);
    default:  // float
        return f.width == 16 ? std::bit_cast<std::uint32_t>(half_to_float(u))
                             : static_cast<std::uint32_t>(u);
    }
}

class TypedBuffer : public testing::Test {
protected:
    void SetUp() override {
        std::string why;
        runner_ = ComputeRunner::create(why);
        if (!runner_) {
            GTEST_SKIP() << why;
        }
        // The table: 16 bytes per lane, the first lanes all 0, all 1 and all 0x80. The second KB
        // repeats it with bit 14 of every 16-bit value cleared, so that a half float is never
        // infinite or NaN, except where the next lines plant them: the 16_16 FLOAT load of
        // offset 1024 reads the first two halves of each lane there.
        std::uint32_t x = 0x2545f491;
        for (std::uint32_t i = 0; i < 1024; ++i) {
            x ^= x << 13, x ^= x >> 17, x ^= x << 5;
            table_[i] = static_cast<std::uint8_t>(x >> 8);
        }
        std::fill_n(table_.begin(), 16, 0);
        std::fill_n(table_.begin() + 16, 16, 0xff);
        std::fill_n(table_.begin() + 32, 16, 0x80);
        std::copy_n(table_.begin(), 1024, table_.begin() + 1024);
        for (std::uint32_t i = 1025; i < table_size; i += 2) {
            table_[i] &= 0xbf;
        }
        const std::uint16_t special[][2] = {
            {0x7c00, 0xfc00},  // +Inf, -Inf
            {0x8000, 0x0000},  // -0, +0
            {0x7e00, 0xfe00},  // quiet NaNs
            {0x7c00, 0x8000},  // +Inf, -0
            {0xfc00, 0x7e00},  // -Inf, NaN
        };
        for (std::uint32_t i = 0; i < std::size(special); ++i) {
            std::memcpy(&table_[1024 + 16 * (4 + i)], special[i], 4);
        }
    }

    // Runs mem_typed with a table V# of `size` bytes and dst_sel `dst_sel`, and checks the 15
    // blocks of output. An element of 4 bytes or more is read from the aligned dwords (the two low
    // bits of its address are ignored), a narrower one byte by byte, and an element whose first
    // byte is outside the buffer reads 0 as a whole, in whatever number format. The V#'s dst_sel
    // never changes the result.
    void check(std::uint32_t size, std::uint32_t dst_sel = identity) {
        const auto program = bb::test::gcn::program_words("mem_typed");
        const auto translation = bb::gcn::translate(
            program, {}, {.pgm_rsrc2 = bb::test::gcn::program_pgm_rsrc2("mem_typed")});
        auto memory = runner_->guest_memory(base, 0x10000);
        std::memcpy(memory.data() + table_offset, table_.data(), table_size);
        const auto lo = [](std::uint64_t v) { return static_cast<std::uint32_t>(v); };
        const auto hi = [](std::uint64_t v) { return static_cast<std::uint32_t>(v >> 32); };
        DrawBlock draw;  // s0, s1 unused; s[4:7] the table's V#, s[8:11] the output's
        draw.user_data = {lo(base), hi(base), 0, 0, lo(base + table_offset),
                          hi(base + table_offset), size, dst_sel, lo(base), hi(base),
                          output_size, identity};
        runner_->run(translation.spirv, draw, {1, 1, 1});
        std::uint32_t k = 0;
        for (const auto& inst : bb::gcn::decode(program)) {
            if (inst.format != bb::gcn::Format::mtbuf) {
                continue;
            }
            const auto layout = fields(inst.dfmt);
            const unsigned bytes = ((layout.back().offset + layout.back().width) + 7) / 8;
            for (std::uint32_t lane = 0; lane < lanes; ++lane) {
                SCOPED_TRACE(testing::Message() << "format " << k << " lane " << lane);
                const std::uint32_t at = 16 * lane + inst.mem_offset;
                // The element is read whole, from the aligned dwords when it has 4 bytes or more.
                const bool inside = at < size;
                std::vector<std::uint8_t> element(bytes);
                for (unsigned j = 0; j < bytes; ++j) {
                    element[j] = inside ? table_[bytes < 4 ? at + j : (at & ~3u) + j] : 0;
                }
                std::array<std::uint32_t, 4> value = {};
                for (unsigned c = 0; c < layout.size() && inside; ++c) {
                    value[c] = component(element, layout[c], inst.nfmt);
                }
                for (unsigned c = 0; c < 4; ++c) {
                    std::uint32_t got;
                    std::memcpy(&got, memory.data() + 1024 * k + 16 * lane + 4 * c, 4);
                    ASSERT_EQ(got, value[c]) << "component " << c;
                }
            }
            ++k;
        }
        EXPECT_EQ(k, 15u);
    }

    std::unique_ptr<ComputeRunner> runner_;
    std::array<std::uint8_t, table_size> table_;
};

TEST_F(TypedBuffer, Formats) {
    check(table_size);
}

// A V# of 648 bytes: lane 40's element starts inside and is read whole although it is cut after
// its second dword, the lanes above read 0 (the SNORM_OGL block too), and so do the loads at
// offset 0x400.
TEST_F(TypedBuffer, Range) {
    check(16 * 40 + 8);
}

// A V# of 643 bytes: the 16-bit element of lane 40 (bytes 642 and 643) starts inside and ends
// beyond the size, and is read whole; the 8_8 element at byte 3 starts outside and reads 0.
TEST_F(TypedBuffer, RangeCutsElement) {
    check(16 * 40 + 3);
}

// MTBUF uses the identity dst_sel (X000 to XYZW by the format) and ignores the V#'s: a dst_sel of
// (W, 1, X, 0), and one of 0, give the same output.
TEST_F(TypedBuffer, DstSelIsIgnored) {
    check(table_size, 7 | 1 << 3 | 4 << 6);
    check(table_size, 0);
}

}  // namespace
