// Execution test of the MUBUF atomics on lavapipe: the program mem_atomic runs each 32-bit atomic
// and five of the _x2 ones on memory of its own per lane, and an inc and an add_x2 that all lanes
// contend for. The results are worked out here from Sea Islands ISA Table 12.8 and 8.1.5.3
// (docs/design/graphics.md, "Resources and memory").

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "gpu/gcn/programs.hpp"
#include "gpu/gcn/translate.hpp"
#include "gpu/runner/compute_runner.hpp"

namespace {

using namespace bb::test::vk;

constexpr std::uint64_t base = 0x12'0000'0000;  // guest address of the memory the atomics update
constexpr std::uint32_t lanes = 64, wide_soffset = 0x1100;  // a row has 36 dwords
constexpr std::uint32_t table_offset = 0x2000, memory_size = 0x5000;

enum class Op { swap, cmpswap, add, sub, smin, umin, smax, umax, and_, or_, xor_, inc, dec,
                fcmpswap, fmin, fmax };
constexpr Op ops32[] = {Op::swap, Op::cmpswap, Op::add,  Op::sub,  Op::smin,     Op::umin,
                        Op::smax, Op::umax,    Op::and_, Op::or_,  Op::xor_,     Op::inc,
                        Op::dec,  Op::fcmpswap, Op::fmin, Op::fmax};
constexpr Op ops64[] = {Op::cmpswap, Op::smax, Op::dec, Op::fmin, Op::fcmpswap};

bool compares(Op op) { return op == Op::cmpswap || op == Op::fcmpswap; }
bool is_float(Op op) { return op >= Op::fcmpswap; }

// The new memory value of `op` on m with src s and cmp c, as Table 12.8 describes it. The floats
// are IEEE values (denormals count): fmin and fmax give the other operand for a NaN and take -0
// as below +0, and fcmpswap finds +0 and -0 equal and a NaN equal to nothing (our reading).
template <class U>
U apply(Op op, U m, U s, U c) {
    using S = std::make_signed_t<U>;
    using F = std::conditional_t<sizeof(U) == 4, float, double>;
    const F fm = std::bit_cast<F>(m), fs = std::bit_cast<F>(s), fc = std::bit_cast<F>(c);
    const bool below = fs < fm || (fs == fm && std::signbit(fs));  // fmin picks src
    const bool above = fs > fm || (fs == fm && !std::signbit(fs));  // fmax picks src
    switch (op) {
    case Op::swap: return s;
    case Op::cmpswap: return m == c ? s : m;
    case Op::add: return m + s;
    case Op::sub: return m - s;
    case Op::smin: return static_cast<S>(s) < static_cast<S>(m) ? s : m;
    case Op::umin: return std::min(s, m);
    case Op::smax: return static_cast<S>(s) > static_cast<S>(m) ? s : m;
    case Op::umax: return std::max(s, m);
    case Op::and_: return m & s;
    case Op::or_: return m | s;
    case Op::xor_: return m ^ s;
    case Op::inc: return m >= s ? 0 : m + 1;
    case Op::dec: return m == 0 || m > s ? s : m - 1;
    case Op::fcmpswap: return fm == fc ? s : m;
    case Op::fmin: return std::isnan(fs) ? m : std::isnan(fm) ? s : below ? s : m;
    case Op::fmax: return std::isnan(fs) ? m : std::isnan(fm) ? s : above ? s : m;
    }
    return m;
}

// Memory, src and cmp of op `k` in lane i: mixed bits, 0 in every fourth lane and src equal to
// the memory in the next; for the floats every pair of 8 values (1.5, -2, +0, -0, inf, NaN, and
// two denormals). cmp equals the memory in the even lanes, and in the odd ones for the floats it
// is the memory with the sign flipped (equal for the zeros only).
template <class U>
std::array<U, 3> operands(Op op, unsigned k, unsigned i) {
    constexpr bool wide = sizeof(U) == 8;
    const U sign = U{1} << (8 * sizeof(U) - 1);
    if (is_float(op)) {
        constexpr std::uint64_t f32[] = {0x3fc0'0000, 0xc000'0000, 0, 0x8000'0000, 0x7f80'0000,
                                         0x7fc0'0000, 1, 0x8000'0003};
        constexpr std::uint64_t f64[] = {0x3ff8ull << 48, 0xc000ull << 48, 0, 1ull << 63,
                                         0x7ff0ull << 48, 0x7ff8ull << 48, 1, 1ull << 63 | 3};
        const auto f = [&](unsigned j) { return static_cast<U>(wide ? f64[j] : f32[j]); };
        return {f(i % 8), f(i / 8), i % 2 == 0 ? f(i % 8) : f(i % 8) ^ sign};
    }
    std::uint64_t x = (k * 64 + i + 1) * 0x9e37'79b9'7f4a'7c15ull;  // splitmix64
    x = (x ^ x >> 30) * 0xbf58'476d'1ce4'e5b9ull;
    x = (x ^ x >> 27) * 0x94d0'49bb'1331'11ebull;
    x ^= x >> 31;
    U m = static_cast<U>(x), s = static_cast<U>(wide ? x * 3 : x >> 32);
    m = i % 4 == 0 ? 0 : m;
    s = i % 4 == 1 ? m : s;
    return {m, s, i % 2 == 0 ? m : static_cast<U>(s ^ x >> 7)};
}

// Each atomic in active lanes and inside the V#, which is cut for the second run inside the
// qwords of fcmpswap_x2: lane 40's qword starts 4 bytes before the end and is updated whole (the
// check is all or nothing, at the first byte), lane 41's is out. An access outside or an
// inactive lane leaves the memory alone, and glc returns 0 outside. The contended inc returns
// each of init .. init + n - 1 once (n active lanes), the add_x2 each init + j * src.
TEST(BufferAtomics, EveryOperation) {
    std::string why;
    const auto runner = ComputeRunner::create(why);
    if (!runner) {
        GTEST_SKIP() << why;
    }
    using namespace bb::test::gcn;
    const auto translation = bb::gcn::translate(
        program_words("mem_atomic"), {},
        {.pgm_rsrc2 = program_pgm_rsrc2("mem_atomic"), .num_threads = {lanes, 1, 1}});
    for (const auto [exec, records] :
         {std::pair{~0ull, 0x2000u}, {0x5555'ffff'ff00'ff0full, 0x1a44u}}) {
        SCOPED_TRACE(testing::Message() << "records " << std::hex << records);
        auto memory = runner->guest_memory(base, memory_size);
        std::vector<std::uint32_t> words(memory_size / 4);
        const auto put = [&](unsigned at, auto v) { std::memcpy(&words[at / 4], &v, sizeof v); };
        const auto get = [&]<class U>(unsigned at, U v) {
            std::memcpy(&v, &words[at / 4], sizeof v);
            return v;
        };
        const auto row = [](unsigned i, unsigned d) { return table_offset + 4 * (36 * i + d); };
        // Each atomic of a lane: its memory and its first row dword; `wide` for the qwords.
        const auto each = [&](auto&& f) {
            for (unsigned i = 0; i < lanes; ++i) {
                unsigned d = 0;
                for (unsigned k = 0; k < std::size(ops32); ++k) {
                    f(std::uint32_t{}, ops32[k], k, i, 0x100 + 0x100 * k + 4 * i, d);
                    d += compares(ops32[k]) ? 2 : 1;
                }
                d = 20;
                for (unsigned k = 0; k < std::size(ops64); ++k) {
                    f(std::uint64_t{}, ops64[k], k, i, wide_soffset + 0x200 * k + 8 * i, d);
                    d += compares(ops64[k]) ? 4 : 2;
                }
            }
        };
        each([&]<class U>(U, Op op, unsigned k, unsigned i, std::uint32_t at, unsigned d) {
            const auto [m, s, c] = operands<U>(op, k, i);
            put(at, m);
            put(row(i, d), s);
            if (compares(op)) {
                put(row(i, d + sizeof(U) / 4), c);
            }
        });
        constexpr std::uint32_t inc_init = 0x7000'0000;
        constexpr std::uint64_t add_init = 0xffff'fff0'0000'0005ull, add_src = 0x1'8000'0000ull;
        put(0, inc_init);
        put(8, add_init);
        for (unsigned i = 0; i < lanes; ++i) {
            put(row(i, 18), ~0u);
            put(row(i, 34), add_src);
        }
        std::memcpy(memory.data(), words.data(), memory_size);
        DrawBlock draw;
        const std::uint64_t table = base + table_offset;
        const auto low = [](std::uint64_t x) { return static_cast<std::uint32_t>(x); };
        draw.user_data = {0, 0, low(exec), low(exec >> 32), low(table), low(table >> 32),
                          lanes * 36 * 4, 0, low(base), low(base >> 32), records, 0,
                          wide_soffset};
        runner->run(translation.spirv, draw, {1, 1, 1});
        std::memcpy(words.data(), memory.data(), memory_size);
        each([&]<class U>(U, Op op, unsigned k, unsigned i, std::uint32_t at, unsigned d) {
            SCOPED_TRACE(testing::Message() << "lane " << std::dec << i << " op " << k << " wide "
                                            << (sizeof(U) == 8));
            const auto [m, s, c] = operands<U>(op, k, i);
            const bool active = (exec >> i & 1) != 0, inside = at < records;
            EXPECT_EQ(get(at, U{}), active && inside ? apply(op, m, s, c) : m);
            const bool glc = op != Op::xor_ || sizeof(U) == 8;
            EXPECT_EQ(get(row(i, d), U{}), !active || !glc ? s : inside ? m : U{0});
            if (compares(op)) {
                EXPECT_EQ(get(row(i, d + sizeof(U) / 4), U{}), c);  // only src is returned
            }
        });
        std::vector<std::uint32_t> incs;
        std::vector<std::uint64_t> adds;
        for (unsigned i = 0; i < lanes; ++i) {
            if ((exec >> i & 1) != 0) {
                incs.push_back(get(row(i, 18), 0u) - inc_init);
                adds.push_back((get(row(i, 34), std::uint64_t{}) - add_init) / add_src);
            } else {
                EXPECT_EQ(get(row(i, 18), 0u), ~0u) << "lane " << i;
            }
        }
        const auto n = static_cast<std::uint32_t>(incs.size());
        std::ranges::sort(incs);
        std::ranges::sort(adds);
        for (std::uint32_t j = 0; j < n; ++j) {
            EXPECT_EQ(incs[j], j);
            EXPECT_EQ(adds[j], j);
        }
        EXPECT_EQ(get(0, 0u), inc_init + n);
        EXPECT_EQ(get(8, std::uint64_t{}), add_init + n * add_src);
    }
}

}  // namespace
