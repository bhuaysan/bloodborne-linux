// Differential tests of the target bank tick wrapper in src/game/ai/: through the registered hook
// the game's tick (0x012a43b0) leaves at 30 FPS the same bytes as without it, and at 60 FPS two
// calls leave what one call leaves at 30.
#include "game/ai/target_bank.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <string_view>
#include <tuple>

#include <gtest/gtest.h>

#include "game/frame/frame.hpp"
#include "game/image.hpp"
#include "harness/eboot.hpp"
#include "harness/harness.hpp"
#include "hook/hook.hpp"

namespace {

using bb::harness::Arena;
using bb::harness::Result;
using bb::test::EbootTest;

constexpr std::uint64_t target_bank_scan_tick = 0x12a43b0;
constexpr std::uint64_t world_chr_man_singleton = 0x553e878;  // WorldChrMan
constexpr std::uint64_t player_singleton = 0x553b148;         // +0x20: the player's chr set

// The wrapper as bb-run registers it (BB_WRAP), hooked for the lifetime of the object.
class WrapperHook {
public:
    WrapperHook() {
        for (const bb::hook::Replacement& r : bb::hook::replacements()) {
            if (r.link_addr == target_bank_scan_tick) {
                hook_[0] = r;
            }
        }
        EXPECT_EQ(hook_[0].impl, reinterpret_cast<void*>(&bb::game::target_bank_scan_tick));
        bb::hook::apply(bb::game::image(), hook_);
    }
    ~WrapperHook() { bb::hook::remove(bb::game::image(), hook_); }
    WrapperHook(const WrapperHook&) = delete;
    WrapperHook& operator=(const WrapperHook&) = delete;

private:
    std::array<bb::hook::Replacement, 1> hook_{};
};

// Sets the frame rate for one test and puts back the default, 30, also when the test fails early.
class FrameRateGuard {
public:
    explicit FrameRateGuard(unsigned fps) { bb::game::set_frame_rate(fps); }
    ~FrameRateGuard() { bb::game::set_frame_rate(bb::game::default_frame_rate); }
    FrameRateGuard(const FrameRateGuard&) = delete;
    FrameRateGuard& operator=(const FrameRateGuard&) = delete;
};

using bb::game::at;

// ---- the world's characters ---------------------------------------------------------------------

// Virtual +0x78 of a character: whether it goes into the bank. Here its byte at +8.
bool fake_is_bankable(void* chr) {
    return at<std::uint8_t>(chr, 8) != 0;
}

struct Case {
    const char* name;
    float interval;                 // +0x28
    float timer;                    // +0x2c
    std::int32_t index;             // +0x94, the ring's slot
    std::int32_t player_set;        // the player singleton's +0x20
    std::uint32_t bank_before = 0;  // entries in the bank before the first call
};

const Case cases[] = {
    // The game's own: 5 s, the timer starts at the interval, so the first call rebuilds the bank.
    {.name = "game start", .interval = 5.0f, .timer = 5.0f, .index = 0, .player_set = 1},
    // A short interval for several rebuilds, the ring wrapping from 99 to 0.
    {.name = "short interval",
     .interval = 0.3f,
     .timer = 0.2f,
     .index = 93,
     .player_set = 1,
     .bank_before = 3},
    // No player chr set (index -1, and one past the end).
    {.name = "no player set", .interval = 0.2f, .timer = 0.0f, .index = 50, .player_set = -1},
    {.name = "player set out of range",
     .interval = 0.2f,
     .timer = 0.0f,
     .index = 99,
     .player_set = 2},
};

// A character with the fake vtable, in or out of the bank.
void* make_chr(Arena& a, void* vtable, bool bankable) {
    void* const chr = a.alloc(0x20);
    at<void*>(chr, 0) = vtable;
    at<std::uint8_t>(chr, 8) = bankable ? 1 : 0;
    return chr;
}

// An array of `n` 0x38-byte entries whose first word is the character (or null).
void* make_entries(Arena& a, std::initializer_list<void*> chrs) {
    void* const entries = a.alloc(0x38 * chrs.size());
    std::size_t i = 0;
    for (void* chr : chrs) {
        at<void*>(entries, 0x38 * i++) = chr;
    }
    return entries;
}

// The SprjTargetBankManager with a ring of bookings, and the world it scans: four characters in
// WorldChrMan's list (one null, one not bankable) and two chr sets of 0x160 bytes, the second with
// two characters. The bank has room for 32 entries, so it never grows (no allocation).
std::tuple<void*, unsigned> build(Arena& a, const Case& c, unsigned calls) {
    void* const vtable = a.alloc(0x100);
    at<void*>(vtable, 0x78) = reinterpret_cast<void*>(&fake_is_bankable);

    void* const world = a.alloc(0x40);
    at<std::int32_t>(world, 0x30) = 4;
    at<void*>(world, 0x38) =
        make_entries(a, {make_chr(a, vtable, true), nullptr, make_chr(a, vtable, false),
                         make_chr(a, vtable, true)});
    void* const sets = a.alloc(2 * 0x160);
    at<std::int32_t>(sets, 0x160 + 0xb8) = 2;
    at<void*>(sets, 0x160 + 0xc0) =
        make_entries(a, {make_chr(a, vtable, true), make_chr(a, vtable, true)});
    at<std::int32_t>(world, 0x20) = 2;
    at<void*>(world, 0x28) = sets;
    bb::game::global<void*>(world_chr_man_singleton) = world;

    void* const player = a.alloc(0x40);
    at<std::int32_t>(player, 0x20) = c.player_set;
    bb::game::global<void*>(player_singleton) = player;

    void* const m = a.alloc(0xa0);
    void* const bank = a.alloc(32 * 8);
    for (std::uint32_t i = 0; i < c.bank_before; ++i) {
        at<std::uint64_t>(bank, 8 * i) = 0x1000 + i;
    }
    at<void*>(m, 0x08) = bank;
    at<void*>(m, 0x10) = static_cast<std::byte*>(bank) + 8 * c.bank_before;
    at<void*>(m, 0x18) = static_cast<std::byte*>(bank) + 32 * 8;
    at<float>(m, 0x28) = c.interval;
    at<float>(m, 0x2c) = c.timer;
    for (int i = 0; i < 100; ++i) {
        at<std::uint8_t>(m, 0x30 + i) = static_cast<std::uint8_t>(i % 4);  // bookings 0 to 3
    }
    at<std::int32_t>(m, 0x94) = c.index;
    at<std::uint8_t>(m, 0x98) = 3;  // at most 3 bookings a slot
    return {m, calls};
}

using Ticks = void(void* manager, unsigned calls);

// `calls` calls of the game's tick, without the hook.
void ticks_unhooked(void* manager, unsigned calls) {
    for (unsigned i = 0; i < calls; ++i) {
        bb::game::fn<bb::game::TargetBankScanTick>(target_bank_scan_tick)(manager);
    }
}

// As many calls through the hook as the frame rate makes of `calls` calls at 30.
void ticks_hooked(void* manager, unsigned calls) {
    const WrapperHook hook;
    // A whole number of calls when `calls` is a multiple of 5 at 144; the wrapper's phase is then
    // back at 0 for the next case.
    const unsigned n = calls * bb::game::frame_rate() / bb::game::default_frame_rate;
    for (unsigned i = 0; i < n; ++i) {
        bb::game::fn<bb::game::TargetBankScanTick>(target_bank_scan_tick)(manager);
    }
}

// 160 calls at 30: over 5 s, so the game's own interval rebuilds the bank twice.
constexpr unsigned calls_at_30 = 160;

// At 30 the hooked tick is the game's, byte for byte.
TEST_F(EbootTest, TargetBankTickUnchangedAt30) {
    for (const Case& c : cases) {
        for (const unsigned calls : {1u, calls_at_30}) {
            const Result r = harness().compare_functions<Ticks>(
                &ticks_unhooked, &ticks_hooked, [&](Arena& a) { return build(a, c, calls); });
            EXPECT_TRUE(r) << c.name << ", " << calls << " calls: " << r.report();
        }
    }
    harness().restore();
}

// At 60 twice as many calls through the hook leave what the game's tick leaves at 30: the same
// timer, the same rebuilds of the bank and the same ring.
TEST_F(EbootTest, TargetBankTickAt60MatchesTheTickAt30) {
    const FrameRateGuard rate(60);
    for (const Case& c : cases) {
        for (const unsigned calls : {1u, calls_at_30}) {
            const Result r = harness().compare_functions<Ticks>(
                &ticks_unhooked, &ticks_hooked, [&](Arena& a) { return build(a, c, calls); });
            EXPECT_TRUE(r) << c.name << ", " << calls << " calls: " << r.report();
        }
    }
    harness().restore();
}

// At 144, 4.8 times as many calls through the hook (the tick 4 or 5 calls apart) leave the same:
// the timer and the ring move on 30 times a game second.
TEST_F(EbootTest, TargetBankTickAt144MatchesTheTickAt30) {
    const FrameRateGuard rate(144);
    for (const Case& c : cases) {
        for (const unsigned calls : {5u, calls_at_30}) {
            const Result r = harness().compare_functions<Ticks>(
                &ticks_unhooked, &ticks_hooked, [&](Arena& a) { return build(a, c, calls); });
            EXPECT_TRUE(r) << c.name << ", " << calls << " calls: " << r.report();
        }
    }
    harness().restore();
}

// Without the wrapper the game's tick at 60 moves twice as fast: the check above sees it.
TEST_F(EbootTest, TargetBankTickUnhookedAt60Differs) {
    const FrameRateGuard rate(60);
    const Case& c = cases[1];
    const auto twice = [](void* manager, unsigned calls) { ticks_unhooked(manager, 2 * calls); };
    const Result r = harness().compare_functions<Ticks>(
        &ticks_unhooked, +twice, [&](Arena& a) { return build(a, c, calls_at_30); });
    EXPECT_FALSE(r);
    harness().restore();
}

TEST_F(EbootTest, TargetBankTickPrologueIsTheEboots) {
    const auto& prologue = bb::game::target_bank_scan_tick_prologue;
    EXPECT_EQ(
        std::memcmp(reinterpret_cast<const void*>(bb::game::image().runtime(target_bank_scan_tick)),
                    prologue.data(), prologue.size()),
        0);
}

// ---- the call pattern ---------------------------------------------------------------------------

int stand_in_calls = 0;
void stand_in(void*) {
    ++stand_in_calls;
}

TEST(TargetBankTick, RunsOnEveryCallsthCall) {
    for (const unsigned calls : {1u, 2u, 4u}) {
        for (const unsigned start : {0u, 1u}) {
            stand_in_calls = 0;
            const unsigned fps = 30 * calls;
            unsigned phase = 30 * (start % calls);
            std::string pattern;
            for (int i = 0; i < 8; ++i) {
                const int before = stand_in_calls;
                bb::game::target_bank_scan_tick_every(&stand_in, fps, phase, nullptr);
                pattern += stand_in_calls > before ? 'x' : '.';
            }
            EXPECT_EQ(stand_in_calls, static_cast<int>(8 / calls)) << calls << " " << start;
            if (calls == 1) {
                EXPECT_EQ(pattern, "xxxxxxxx");
                EXPECT_EQ(phase, 0u);
            } else if (calls == 2) {
                EXPECT_EQ(pattern, start == 0 ? "x.x.x.x." : ".x.x.x.x");
            } else {
                EXPECT_EQ(pattern, start == 0 ? "x...x..." : "...x...x");
            }
        }
    }
}

// At 144 the tick runs 4 or 5 calls apart, 30 times in 144 calls (a game second), whatever the
// phase it starts at.
TEST(TargetBankTick, RunsThirtyTimesAGameSecondAt144) {
    for (const unsigned start : {0u, 6u, 29u, 30u, 143u}) {
        stand_in_calls = 0;
        unsigned phase = start;
        int last = -1;
        for (int i = 0; i < 144; ++i) {
            const int before = stand_in_calls;
            bb::game::target_bank_scan_tick_every(&stand_in, 144, phase, nullptr);
            if (stand_in_calls > before) {
                if (last >= 0) {
                    EXPECT_TRUE(i - last == 4 || i - last == 5) << start << " " << i;
                }
                last = i;
            }
        }
        EXPECT_EQ(stand_in_calls, 30) << start;
        EXPECT_EQ(phase, start);  // 144 calls move the phase on by 144 * 30, a whole turn
    }
}

// A phase left over from 60 FPS doesn't skip a call at 30.
TEST(TargetBankTick, RunsEveryCallAt30WhateverThePhase) {
    stand_in_calls = 0;
    unsigned phase = 30;
    bb::game::target_bank_scan_tick_every(&stand_in, 30, phase, nullptr);
    EXPECT_EQ(stand_in_calls, 1);
    EXPECT_EQ(phase, 0u);
}

TEST(TargetBankTick, IsRegisteredWithItsPrologue) {
    auto all = bb::hook::replacements();
    auto it = std::find_if(all.begin(), all.end(), [](const bb::hook::Replacement& r) {
        return std::string_view(r.name) == "bb::game::target_bank_scan_tick";
    });
    ASSERT_NE(it, all.end());
    EXPECT_EQ(it->link_addr, 0x012a43b0U);
    ASSERT_EQ(it->prologue.size(), bb::game::target_bank_scan_tick_prologue.size());
    EXPECT_TRUE(std::equal(it->prologue.begin(), it->prologue.end(),
                           bb::game::target_bank_scan_tick_prologue.begin()));
}

}  // namespace
