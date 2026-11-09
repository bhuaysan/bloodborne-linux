// Execution tests of the typed buffer loads and stores (MTBUF) on lavapipe: the program mem_typed
// loads 17 formats from a table, and each result is worked out here from the element's bytes with
// the usual conversions of normalized, scaled and integer formats; mem_tstore stores 14 formats,
// and the output is worked out from the conversions back (docs/design/graphics.md, "Typed buffer
// formats"). FormattedBuffer runs mem_fmt, the formatted MUBUF forms, for each V# format pair.

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

#include "gpu/gcn/buffer_format.hpp"
#include "gpu/gcn/gcn.hpp"
#include "gpu/gcn/programs.hpp"
#include "gpu/gcn/translate.hpp"
#include "gpu/runner/compute_runner.hpp"

namespace {

using namespace bb::test::vk;
namespace nf {
constexpr unsigned unorm = 0, snorm = 1, uscaled = 2, sscaled = 3, uint_ = 4, sint = 5, snorm_ogl = 6;
}  // the number formats the programs use

constexpr std::uint64_t base = 0x12'0000'0000;  // guest address of the output
constexpr std::uint32_t table_offset = 0x8000, table_size = 2048, lanes = 64;
constexpr std::uint32_t output_size = 0x4400;  // one block of 1 KB per format
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
    case 4: return {{0, 32}};
    case 5: return {{0, 16}, {16, 16}};
    case 6: return {{0, 11}, {11, 11}, {22, 10}};
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

// The finite unsigned small float `code` with 5 exponent bits (bias 15) and `m` mantissa bits.
double small_float(std::uint32_t code, unsigned m) {
    const unsigned e = code >> m, mantissa = code & ((1u << m) - 1);
    return std::ldexp(e == 0 ? mantissa : mantissa + (1 << m), int(std::max(e, 1u)) - 15 - int(m));
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
    case nf::uscaled: return std::bit_cast<std::uint32_t>(static_cast<float>(u));
    case nf::sscaled: return std::bit_cast<std::uint32_t>(static_cast<float>(s));
    case nf::uint_: return static_cast<std::uint32_t>(u);
    case nf::sint: return static_cast<std::uint32_t>(s);
    case nf::snorm_ogl:
        return std::bit_cast<std::uint32_t>(static_cast<float>(2 * s + 1) / max);
    default:  // float
        if (f.width == 10 || f.width == 11) {  // a NaN becomes the quiet f32 NaN
            const unsigned m = f.width - 5;
            return u >> m != 31          ? std::bit_cast<std::uint32_t>(float(small_float(u, m)))
                   : (u & ((1u << m) - 1)) ? 0x7fc00000u
                                           : 0x7f800000u;
        }
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

    // Runs mem_typed with a table V# of `size` bytes and dst_sel `dst_sel`, and checks the 16
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
        EXPECT_EQ(k, 17u);
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

// The `width` bits that a store writes for the component `v` (a float's bits or an integer):
// UNORM and SNORM clamp (NaN gives 0), scale in f32 and round to nearest even, UINT and SINT
// saturate, and FLOAT rounds to a half float to nearest even (a NaN gives the quiet NaN of its
// sign) or keeps the 32 bits.
std::uint32_t encode(std::uint32_t v, unsigned width, unsigned nfmt) {
    const std::uint64_t mask = (std::uint64_t{1} << width) - 1;
    float f = std::bit_cast<float>(v);
    f = std::isnan(f) ? 0.0f : f;
    const auto s = static_cast<std::int64_t>(static_cast<std::int32_t>(v));
    const std::int64_t smax = (std::int64_t{1} << (width - 1)) - 1;
    switch (nfmt) {
    case nf::unorm:
        return static_cast<std::uint32_t>(
            std::nearbyint(std::clamp(f, 0.0f, 1.0f) * static_cast<float>(mask)));
    case nf::snorm:
        return static_cast<std::uint32_t>(static_cast<std::int64_t>(std::nearbyint(
                   std::clamp(f, -1.0f, 1.0f) * static_cast<float>(smax)))) & mask;
    case nf::uint_: return static_cast<std::uint32_t>(std::min<std::uint64_t>(v, mask));
    case nf::sint: return static_cast<std::uint32_t>(std::clamp(s, -smax - 1, smax)) & mask;
    default:
        if (width == 32) {
            return v;
        }
        if (width < 16) {  // the unsigned small floats
            const unsigned m = width - 5, infinity = 31u << m;
            const float x = std::bit_cast<float>(v);
            if (std::isnan(x) || std::isinf(x) || std::signbit(x) || x == 0) {
                return std::isnan(x) ? infinity | 1u << (m - 1) : x > 0 ? infinity : 0;
            }
            std::uint32_t best = 0;  // the nearest finite code, ties to the even one
            for (std::uint32_t code = 1; code < infinity; ++code) {
                const double d = std::abs(small_float(code, m) - x),
                             b = std::abs(small_float(best, m) - x);
                best = d < b || (d == b && code % 2 == 0) ? code : best;
            }
            return best;
        }
        if (std::isnan(std::bit_cast<float>(v))) {
            return (v >> 16 & 0x8000) | 0x7e00;
        }
        return std::bit_cast<std::uint16_t>(static_cast<_Float16>(std::bit_cast<float>(v)));
    }
}

class TypedStore : public testing::Test {
protected:
    void SetUp() override {
        std::string why;
        runner_ = ComputeRunner::create(why);
        if (!runner_) {
            GTEST_SKIP() << why;
        }
        // Floats: the edges of the conversions first (clamping, the ties of round to even, the
        // half float's range and denormals), then random values around 1 and in the half
        // float's range. Integers: the edges of 2 to 16 bits, then random values of any size.
        const float specials[] = {
            0.0f, -0.0f, 1.0f, -1.0f, 0.5f, 1.5f, 2.0f, -2.0f,
            std::numeric_limits<float>::quiet_NaN(), -std::numeric_limits<float>::quiet_NaN(),
            std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
            0.5f / 255, 1.5f / 255, 2.5f / 255, -0.5f / 127, -1.5f / 127, 0.5f / 511,
            65504.0f, 65519.0f, 65520.0f, 65536.0f, -65520.0f, 1e10f,
            std::ldexp(1.0f, -14), std::ldexp(1023.0f, -24), std::ldexp(2047.0f, -25),
            std::ldexp(1.0f, -24), std::ldexp(1.0f, -25), std::ldexp(3.0f, -26),
            std::ldexp(3.0f, -25), std::ldexp(1.0f, -26), -std::ldexp(1.0f, -25),
            1.0f + std::ldexp(1.0f, -11), 1.0f + std::ldexp(3.0f, -11), 1e-30f, -1e-40f,
        };
        const std::int32_t ints[] = {
            0, 1, -1, 2, -2, -3, 3, 127, 128, -128, -129, 255, 256, 511, 512, -512, -513, 1023,
            1024, 32767, 32768, -32768, -32769, 65535, 65536, INT32_MIN, INT32_MAX, -65536,
        };
        std::uint32_t x = 0x9e3779b9;
        const auto next = [&] { return x ^= x << 13, x ^= x >> 17, x ^= x << 5; };
        for (std::uint32_t i = 0; i < 4 * lanes; ++i) {
            const std::uint32_t r = next();
            const float f = i < std::size(specials) ? specials[i]
                            : i % 2 != 0
                                ? std::ldexp(static_cast<float>(r % 4096) - 2048, -10)
                                : std::bit_cast<float>((r & 0x807fffff) | (100 + r % 48) << 23);
            input_[i] = std::bit_cast<std::uint32_t>(f);
            input_[4 * lanes + i] = i < std::size(ints)
                                        ? static_cast<std::uint32_t>(ints[i])
                                        : static_cast<std::int32_t>(next()) >> (r % 32);
        }
    }

    // Runs mem_tstore with an output V# of `size` bytes, over output bytes that start as 0xa5,
    // and checks every byte of the output. Each store writes its element whole when its first
    // byte is inside the V# and nothing otherwise; an element of 4 bytes or more is written to
    // the aligned dwords.
    void check(std::uint32_t size) {
        const auto program = bb::test::gcn::program_words("mem_tstore");
        const auto translation = bb::gcn::translate(
            program, {}, {.pgm_rsrc2 = bb::test::gcn::program_pgm_rsrc2("mem_tstore")});
        auto memory = runner_->guest_memory(base, 0x10000);
        std::vector<std::uint8_t> expected(output_size, 0xa5);
        std::memcpy(memory.data(), expected.data(), output_size);
        std::memcpy(memory.data() + table_offset, input_.data(), sizeof(input_));
        const auto lo = [](std::uint64_t v) { return static_cast<std::uint32_t>(v); };
        const auto hi = [](std::uint64_t v) { return static_cast<std::uint32_t>(v >> 32); };
        DrawBlock draw;  // s[4:7] the input's V#, s[8:11] the output's
        draw.user_data = {lo(base), hi(base), 0, 0, lo(base + table_offset),
                          hi(base + table_offset), sizeof(input_), identity, lo(base), hi(base),
                          size, identity};
        runner_->run(translation.spirv, draw, {1, 1, 1});
        std::uint32_t k = 0;
        for (const auto& inst : bb::gcn::decode(program)) {
            if (inst.format != bb::gcn::Format::mtbuf) {
                continue;
            }
            const auto layout = fields(inst.dfmt);
            const unsigned bytes = (layout.back().offset + layout.back().width + 7) / 8;
            const unsigned integer = inst.nfmt == nf::uint_ || inst.nfmt == nf::sint ? 1 : 0;
            const unsigned scale = inst.src[1].code == 0 ? 1 : inst.src[1].code == 2 ? 2 : 16;
            for (std::uint32_t lane = 0; lane < lanes; ++lane) {
                std::uint32_t at = 1024 * k + scale * lane + inst.mem_offset;
                if (at >= size) {
                    continue;
                }
                at = bytes < 4 ? at : at & ~3u;
                std::uint64_t element[2] = {};
                for (unsigned c = 0; c < layout.size(); ++c) {
                    const std::uint32_t v =
                        c < inst.src[0].dwords ? input_[4 * (lanes * integer + lane) + c] : 0;
                    const std::uint64_t bits = encode(v, layout[c].width, inst.nfmt);
                    element[layout[c].offset / 64] |= bits << (layout[c].offset % 64);
                }
                std::memcpy(expected.data() + at, element, bytes);
            }
            ++k;
        }
        EXPECT_EQ(k, 14u);
        for (std::uint32_t i = 0; i < output_size; ++i) {
            ASSERT_EQ(static_cast<unsigned>(memory[i]), expected[i])
                << "block " << i / 1024 << " byte " << i % 1024;
        }
    }

    std::unique_ptr<ComputeRunner> runner_;
    std::array<std::uint32_t, 8 * lanes> input_;
};

// Every lane's element is written, in each format; the narrow elements of adjacent lanes share
// dwords, and half of the 8_8 elements cross into the next dword.
TEST_F(TypedStore, Formats) {
    check(output_size);
}

// A V# that ends at byte 42 of the 8_8 block: lane 20's element (bytes 41 and 42) starts inside
// and is written whole, the next lanes and the later blocks are not written.
TEST_F(TypedStore, Range) {
    check(1024 * 4 + 42);
}

// mem_fmt: the formatted MUBUF loads and stores take the format and dst_sel from the V#'s dword 3,
// at run time. The loads read TypedStore's floats, and the stores write its inputs.
class FormattedBuffer : public TypedStore {
protected:
    void SetUp() override {
        TypedStore::SetUp();
        program_ = bb::test::gcn::program_words("mem_fmt");
        for (const auto& inst : bb::gcn::decode(program_)) {  // the tfe bit of the MTBUF load
            if (inst.format == bb::gcn::Format::mtbuf) {
                program_[inst.offset / 4 + 1] |= 1u << 23;
            }
        }
        spirv_ = bb::gcn::translate(
                     program_, {}, {.pgm_rsrc2 = bb::test::gcn::program_pgm_rsrc2("mem_fmt")})
                     .spirv;
    }

    // Runs mem_fmt with the V# format (dfmt, nfmt), dst_sel `sel` and size `size`, and checks the
    // loads, the tfe VGPRs and every byte of the store region. A pair that is not one of
    // formatted_pairs reads 0 whatever dst_sel says and writes nothing. A load returns the
    // components through dst_sel (1 as 1.0, or 1 in UINT and SINT); a store gives memory
    // component k the first input component whose dst_sel is 4 + k, or 0. The stores' lane stride
    // is the element's size, so the 1-byte elements of adjacent lanes share a dword.
    void check(unsigned dfmt, unsigned nfmt, std::uint32_t sel, std::uint32_t size) {
        SCOPED_TRACE(testing::Message() << "dfmt " << dfmt << " nfmt " << nfmt);
        const bool reads = std::ranges::count(bb::gcn::formatted_pairs,
                                              std::array<unsigned, 2>{dfmt, nfmt}) != 0;
        const std::vector<Field> layout = reads ? fields(dfmt) : std::vector<Field>{};
        const unsigned bytes =
            reads ? (layout.back().offset + layout.back().width + 7) / 8 : 16;
        const unsigned shift = std::bit_width(bytes - 1);  // the lane stride: 2^shift >= bytes
        const bool integer = nfmt == nf::uint_ || nfmt == nf::sint;
        auto memory = runner_->guest_memory(base, 0x10000);
        std::memset(memory.data(), 0xa5, 0x800);
        const auto* table = reinterpret_cast<const std::uint8_t*>(input_.data());
        std::memcpy(memory.data() + table_offset, table, 0x400);
        std::memset(memory.data() + table_offset + 0xc00, 0xa5, 0x400);
        std::memcpy(memory.data() + table_offset + 0x800, input_.data() + (integer ? 256 : 0),
                    0x400);
        std::vector<std::uint8_t> region(0x1000);
        std::memcpy(region.data(), memory.data() + table_offset, region.size());
        const auto lo = [](std::uint64_t v) { return static_cast<std::uint32_t>(v); };
        DrawBlock draw;  // s[4:7] the formatted V#, s[8:11] the output's, s12 the stride's log2
        draw.user_data = {0, 0, 0, 0, lo(base + table_offset), lo(base >> 32), size,
                          sel | nfmt << 12 | dfmt << 15, lo(base), lo(base >> 32), 0x800,
                          identity, shift};
        runner_->run(spirv_, draw, {1, 1, 1});
        const auto word = [&](const void* p) {
            std::uint32_t v;
            std::memcpy(&v, p, 4);
            return v;
        };
        for (std::uint32_t lane = 0; lane < lanes; ++lane) {
            SCOPED_TRACE(testing::Message() << "lane " << lane);
            const bool inside = 16 * lane < size;
            std::array<std::uint32_t, 6> value = {0, integer ? 1u : 0x3f800000u};
            const std::vector<std::uint8_t> loaded(table + 16 * lane, table + 16 * lane + bytes);
            for (unsigned c = 0; c < layout.size() && inside; ++c) {
                value[2 + c] = component(loaded, layout[c], nfmt);
            }
            for (unsigned c = 0; c < 4; ++c) {
                const unsigned s = sel >> (3 * c) & 7;
                const std::uint32_t want = reads ? value[s >= 4 ? s - 2 : s == 1] : 0;
                ASSERT_EQ(word(&memory[16 * lane + 4 * c]), want) << "component " << c;
            }
            EXPECT_EQ(word(&memory[0x400 + 4 * lane]), 0u);  // tfe: no fetch fails
            EXPECT_EQ(word(&memory[0x600 + 8 * lane]), inside ? word(table + 16 * lane) : 0);
            EXPECT_EQ(word(&memory[0x604 + 8 * lane]), 0u);
            const std::uint32_t at = 0xc00 + (lane << shift);
            if (!reads || at >= size) {
                continue;
            }
            std::uint64_t element[2] = {};
            for (unsigned k = 0; k < layout.size(); ++k) {
                std::uint32_t chosen = 0;
                for (unsigned c = 4; c-- > 0;) {  // the first component that selects k
                    const std::uint32_t d = 0x800 + 16 * lane + 4 * c;
                    const std::uint32_t v = d < size ? word(&region[d]) : 0;
                    chosen = (sel >> (3 * c) & 7) == 4 + k ? v : chosen;
                }
                const std::uint64_t encoded = encode(chosen, layout[k].width, nfmt);
                element[layout[k].offset / 64] |= encoded << (layout[k].offset % 64);
            }
            std::memcpy(region.data() + at, element, bytes);
        }
        for (std::uint32_t i = 0xc00; i < 0x1000; ++i) {
            ASSERT_EQ(static_cast<unsigned>(memory[table_offset + i]), region[i]) << "byte " << i;
        }
    }

    std::vector<std::uint32_t> program_, spirv_;
};

// Every pair of the 16 data and 8 number formats, with the identity dst_sel: those of
// formatted_pairs convert, the others read 0 and write nothing.
TEST_F(FormattedBuffer, AllPairs) {
    for (unsigned key = 0; key < 128; ++key) {
        check(key >> 3, key & 7, identity, 0x1000);
    }
}

// The V#'s dst_sel (W, 1, X, 0) reorders a load's components and, inverted, a store's; a pair
// without a case reads 0 whatever dst_sel says.
TEST_F(FormattedBuffer, DstSel) {
    for (const auto [dfmt, nfmt] : {std::pair{14u, 7u}, {4u, nf::uint_}, {11u, 7u}, {13u, 7u},
                                    {10u, nf::unorm}, {0u, nf::uint_}}) {
        check(dfmt, nfmt, 7 | 1 << 3 | 4 << 6, 0x1000);
    }
}

// The dst_sel and format pairs of the game's compute programs (measured, see the design note):
// BGRA and RGBA on 8_8_8_8 UNORM, 000R and R001 on 8 UNORM, R001 on 32 FLOAT, RG01 on 32_32
// FLOAT, XYZ0 on 32_32_32 UINT, RGBA on 32_32_32_32 FLOAT and UINT.
TEST_F(FormattedBuffer, GameDstSels) {
    constexpr std::uint32_t bgra = 6 | 5 << 3 | 4 << 6 | 7 << 9, r001 = 4 | 1 << 9;
    constexpr std::uint32_t r_in_w = 4 << 9, rg01 = 4 | 5 << 3 | 1 << 9, xyz0 = 4 | 5 << 3 | 6 << 6;
    check(10, nf::unorm, bgra, 0x1000);
    check(10, nf::unorm, identity, 0x1000);
    check(1, nf::unorm, r_in_w, 0x1000);
    check(1, nf::unorm, r001, 0x1000);
    check(4, 7, r001, 0x1000);
    check(11, 7, rg01, 0x1000);
    check(13, nf::uint_, xyz0, 0x1000);
    check(14, 7, identity, 0x1000);
    check(14, nf::uint_, identity, 0x1000);
}

// A V# of 648 bytes: lane 40's element is read whole, the lanes above load 0, and no store is
// inside. One that ends 1 byte into lane 20's stored element: that element is written whole, the
// later ones not at all, also for the 1-byte elements, whose adjacent lanes share a dword.
TEST_F(FormattedBuffer, Range) {
    check(14, 7, identity, 16 * 40 + 8);
    check(4, nf::sint, identity, 0xc00 + 20 * 4 + 1);
    check(13, 7, identity, 0xc00 + 20 * 16 + 1);
    check(10, nf::unorm, identity, 0xc00 + 20 * 4 + 1);
    check(1, nf::unorm, identity, 0xc00 + 20 + 1);
}

}  // namespace
