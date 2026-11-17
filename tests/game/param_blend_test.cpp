// Differential tests of the int parameter blend in src/game/gfx/param_blend.cpp: at 30 FPS the
// replacement matches the game's function bit for bit, at 60 FPS an entry equal to the current
// value keeps it (#598).
#include "game/gfx/param_blend.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <tuple>

#include <gtest/gtest.h>

#include "game/frame/frame.hpp"
#include "harness/eboot.hpp"
#include "harness/harness.hpp"

namespace {

using bb::game::ParamEntry;
using bb::harness::Arena;
using bb::harness::Result;
using bb::test::EbootTest;

using BlendFn = bool(const void*, std::int32_t, float, const std::int32_t*, std::int32_t*);

constexpr std::uint64_t param_blend_int = 0x029262f0;

struct FrameRateGuard {
    explicit FrameRateGuard(unsigned fps) { bb::game::set_frame_rate(fps); }
    ~FrameRateGuard() { bb::game::set_frame_rate(bb::game::default_frame_rate); }
};

// A list of three entries (ids 7, 5 with `value`, 9) in an object at +0x30, the current value
// and an output slot that holds a marker, so a call that writes nothing shows.
auto blend_args(std::int32_t id, float t, std::int32_t value, std::int32_t current) {
    return [=](Arena& a) {
        auto* self = a.zeroed<std::uint8_t>(0x40);
        auto* entries = a.zeroed<ParamEntry>(3);
        const std::int32_t ids[] = {7, 5, 9};
        for (int i = 0; i < 3; ++i) {
            entries[i].id = ids[i];
            entries[i].value = ids[i] == 5 ? value : 1000 + i;
            entries[i].next = i < 2 ? &entries[i + 1] : nullptr;
        }
        ParamEntry* head = entries;
        std::memcpy(self + 0x30, &head, sizeof(head));
        auto* cur = a.zeroed<std::int32_t>();
        *cur = current;
        auto* out = a.zeroed<std::int32_t>();
        *out = 0x5a5a5a5a;
        return std::tuple<const void*, std::int32_t, float, const std::int32_t*, std::int32_t*>{
            self, id, t, cur, out};
    };
}

constexpr float nan = std::numeric_limits<float>::quiet_NaN();
constexpr float inf = std::numeric_limits<float>::infinity();
const float ts[] = {0.0f,   -0.0f,     -1.0f,     1.0f, 2.0f,
                    0.5f,   1.0f / 30, 1.0f / 15, 0.2f, 0.999999f,
                    1e-30f, nan,       inf,       -inf, std::nextafter(1.0f, 0.0f)};
const std::int32_t values[] = {
    0, 1, 2, 3, 4, 8, -1, 1110249832, 0x7fffffff, std::numeric_limits<std::int32_t>::min()};

TEST_F(EbootTest, ParamBlendIntMatchesAt30) {
    EXPECT_EQ(image().function_size(param_blend_int), std::optional<std::uint64_t>{101});
    for (std::int32_t id : {5, 7, 9, 6}) {
        for (float t : ts) {
            for (std::int32_t value : values) {
                for (std::int32_t current : values) {
                    const Result r =
                        harness().compare<BlendFn>(param_blend_int, &bb::game::param_blend_int,
                                                   blend_args(id, t, value, current));
                    EXPECT_TRUE(r) << "id " << id << " t " << t << " value " << value << " current "
                                   << current << ": " << r.report();
                }
            }
        }
    }
}

TEST_F(EbootTest, ParamBlendIntWithAnEmptyListWritesNothing) {
    const auto empty = [](Arena& a) {
        auto* cur = a.zeroed<std::int32_t>();
        auto* out = a.zeroed<std::int32_t>();
        *out = 0x5a5a5a5a;
        return std::tuple<const void*, std::int32_t, float, const std::int32_t*, std::int32_t*>{
            a.zeroed<std::uint8_t>(0x40), 5, 0.5f, cur, out};
    };
    const Result r = harness().compare<BlendFn>(param_blend_int, &bb::game::param_blend_int, empty);
    EXPECT_TRUE(r) << r.report();
}

// At 60 FPS unequal values blend as the game's function does.
TEST_F(EbootTest, ParamBlendIntMatchesUnequalValuesAt60) {
    const FrameRateGuard at60(60);
    for (float t : ts) {
        for (std::int32_t value : values) {
            for (std::int32_t current : values) {
                if (value == current) {
                    continue;
                }
                const Result r = harness().compare<BlendFn>(
                    param_blend_int, &bb::game::param_blend_int, blend_args(5, t, value, current));
                EXPECT_TRUE(r) << "t " << t << " value " << value << " current " << current << ": "
                               << r.report();
            }
        }
    }
}

// The fault of #598: the game's blend at the 60 FPS step turns 3 into 2.
TEST_F(EbootTest, ParamBlendIntKeepsAnEqualValueAt60) {
    const auto original = image().fn<BlendFn>(param_blend_int);
    std::uint8_t self[0x40] = {};
    ParamEntry entry{};
    entry.id = 5;
    entry.value = 3;
    ParamEntry* head = &entry;
    std::memcpy(self + 0x30, &head, sizeof(head));
    const std::int32_t current = 3;
    std::int32_t out = 0;
    ASSERT_TRUE(original(self, 5, 1.0f / 60 * 2, &current, &out));
    EXPECT_EQ(out, 2);

    const FrameRateGuard at60(60);
    for (std::int32_t v : values) {
        entry.value = v;
        for (float t : ts) {
            out = 0x5a5a5a5a;
            ASSERT_TRUE(bb::game::param_blend_int(self, 5, t, &v, &out));
            EXPECT_EQ(out, v) << "t " << t;
        }
    }
    EXPECT_FALSE(bb::game::param_blend_int(self, 6, 0.5f, &current, &out));
}

}  // namespace
