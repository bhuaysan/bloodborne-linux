// Differential tests of the character setup pacing in src/game/chr/: through the registered hook
// the game's setup gate (0x018b9890) does at 30 FPS what it does without it, and at 60 FPS, on a
// frame whose counter is even, the same, and on an odd one what it does with a spent budget.
// Characters waiting for their setup are then set up at the same rate a game second at both rates.
#include "game/chr/validate_pace.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "game/frame/frame.hpp"
#include "game/image.hpp"
#include "harness/eboot.hpp"
#include "hook/hook.hpp"

namespace {

using bb::game::ValidateBudget;
using bb::hook::Replacement;
using bb::test::EbootTest;

constexpr std::uint64_t chr_validate = 0x018b9890;

// The globals the gate reads on its way to the budget: a debug flags object (+0x15a2 bit 0 asks
// for WorldChrManDbg), a byte that makes it call 0x01a17980 for a character without a behaviour
// slot, and the WorldChrManDbg, SprjBehavior and SprjHkBehManager singletons, which it only checks
// for null and reads the last for a character with a behaviour slot: +0x40 -> {entries at +0x10,
// 0x30 bytes each, their count at +0x28}, the slot's state at +0x20 of entry (id - 1) % count.
constexpr std::uint64_t debug_flags = 0x05556678;
constexpr std::uint64_t world_chr_man_dbg = 0x0553e880;
constexpr std::uint64_t behaviour_call_flag = 0x05128343;
constexpr std::uint64_t sprj_behavior = 0x05540210;
constexpr std::uint64_t hk_beh_manager = 0x055401f8;

// A character as far as the gate reads it: a vtable whose +0x1b8 says whether the budget skips it
// and whose +0x60 sets it up, its behaviour (+0x3b0 -> +0x30 -> slot id at +0x30c, 0: none), the
// flags at +0x1e1 and the object at +0x288.
struct FakeChr {
    const void* const* vtable;
    std::array<std::byte, 0x400> fields{};
    bool skips_budget = false;
    int setups = 0;
};
static_assert(offsetof(FakeChr, fields) == 8);

bool fake_skips_budget(FakeChr* chr) {
    return chr->skips_budget;
}
void fake_setup(FakeChr* chr) {
    ++chr->setups;
}

std::array<const void*, 0x1c0 / 8> fake_vtable = [] {
    std::array<const void*, 0x1c0 / 8> v{};
    v[0x60 / 8] = reinterpret_cast<const void*>(&fake_setup);
    v[0x1b8 / 8] = reinterpret_cast<const void*>(&fake_skips_budget);
    return v;
}();

template <class T>
T& field(FakeChr& chr, std::size_t offset) {
    return *reinterpret_cast<T*>(reinterpret_cast<std::byte*>(&chr) + offset);
}

Replacement registered(std::uint64_t link) {
    for (const Replacement& r : bb::hook::replacements()) {
        if (r.link_addr == link) {
            return r;
        }
    }
    ADD_FAILURE() << "nothing registered at " << link;
    return {};
}

class ChrValidatePaceTest : public EbootTest {
protected:
    void SetUp() override {
        EbootTest::SetUp();
        if (IsSkipped()) {
            return;
        }
        saved_ = {global(debug_flags), global(sprj_behavior), global(hk_beh_manager),
                  global(world_chr_man_dbg), global(bb::game::world_chr_man)};
        saved_flag_ = bb::game::global<std::uint8_t>(behaviour_call_flag);
        global(debug_flags) = reinterpret_cast<std::uintptr_t>(debug_.data());
        global(sprj_behavior) = reinterpret_cast<std::uintptr_t>(singleton_.data());
        global(hk_beh_manager) = reinterpret_cast<std::uintptr_t>(beh_manager_.data());
        std::byte* const table = beh_manager_.data() + 0x100;
        std::byte* const entries = beh_manager_.data() + 0x200;
        const std::uint32_t slots = 3;
        std::memcpy(beh_manager_.data() + 0x40, &table, sizeof table);
        std::memcpy(table + 0x10, &entries, sizeof entries);
        std::memcpy(table + 0x28, &slots, sizeof slots);
        global(world_chr_man_dbg) = reinterpret_cast<std::uintptr_t>(singleton_.data());
        global(bb::game::world_chr_man) = reinterpret_cast<std::uintptr_t>(world_.data());
        bb::game::global<std::uint8_t>(behaviour_call_flag) = 0;
        chr_.vtable = fake_vtable.data();
        field<void*>(chr_, 0x3b0) = behaviour_.data();
        std::byte* const inner = behaviour_.data() + 0x40;
        std::memcpy(behaviour_.data() + 0x30, &inner, sizeof inner);  // slot id at +0x30c: 0
        hooked_ = {registered(chr_validate)};
    }
    void TearDown() override {
        if (!saved_.empty()) {
            global(debug_flags) = saved_[0];
            global(sprj_behavior) = saved_[1];
            global(hk_beh_manager) = saved_[2];
            global(world_chr_man_dbg) = saved_[3];
            global(bb::game::world_chr_man) = saved_[4];
            bb::game::global<std::uint8_t>(behaviour_call_flag) = saved_flag_;
            bb::game::set_frame_rate(bb::game::default_frame_rate);
            bb::game::set_frame_count(0);
        }
    }

    static std::uintptr_t& global(std::uint64_t link) {
        return bb::game::global<std::uintptr_t>(link);
    }
    // A main loop frame in which the world's time goes on: WorldChrMan's counter is `frame`.
    void set_frame(std::uint32_t frame) {
        bb::game::set_frame_count(bb::game::frame_count() + 1);
        set_counter(frame);
    }
    void set_counter(std::uint32_t counter) {
        std::memcpy(world_.data() + bb::game::world_chr_man_frame, &counter, sizeof counter);
    }
    // Gives the character behaviour slot `id` (0: none) whose state is `state`.
    void set_slot(std::uint32_t id, std::uint32_t state) {
        std::memcpy(behaviour_.data() + 0x40 + 0x30c, &id, sizeof id);
        if (id != 0) {
            std::memcpy(beh_manager_.data() + 0x200 + (id - 1) % 3 * 0x30 + 0x20, &state,
                        sizeof state);
        }
    }
    // One call of the gate at its address, through the hook when `hooked`.
    struct Call {
        bool result;
        ValidateBudget budget;
        int setups;
    };
    Call call(bool hooked, ValidateBudget budget) {
        if (hooked) {
            bb::hook::apply(bb::game::image(), hooked_);
        }
        const int before = chr_.setups;
        const bool result = bb::game::fn<bool(ValidateBudget*, void*)>(chr_validate)(
            &budget, null_chr_ ? nullptr : &chr_);
        if (hooked) {
            bb::hook::remove(bb::game::image(), hooked_);
        }
        return {result, budget, chr_.setups - before};
    }

    std::vector<std::uintptr_t> saved_;
    std::uint8_t saved_flag_ = 0;
    std::vector<std::byte> debug_ = std::vector<std::byte>(0x2000);
    std::vector<std::byte> singleton_ = std::vector<std::byte>(0x100);
    std::vector<std::byte> beh_manager_ = std::vector<std::byte>(0x400);
    std::vector<std::byte> world_ = std::vector<std::byte>(0x1000);
    bool null_chr_ = false;
    std::vector<std::byte> behaviour_ = std::vector<std::byte>(0x400);
    FakeChr chr_{};
    std::array<Replacement, 1> hooked_{};
};

constexpr ValidateBudget budgets[] = {{-1, 0}, {-1, 5}, {0, 0}, {1, 0}, {1, 1},
                                      {1, 2},  {2, 1},  {3, 2}, {3, 3}};

void expect_same(const auto& a, const auto& b, const char* what) {
    EXPECT_EQ(a.result, b.result) << what;
    EXPECT_EQ(a.budget.limit, b.budget.limit) << what;
    EXPECT_EQ(a.budget.count, b.budget.count) << what;
    EXPECT_EQ(a.setups, b.setups) << what;
}

TEST_F(ChrValidatePaceTest, IsTheGamesGateAt30) {
    bb::game::set_frame_rate(30);
    for (const bool skips : {false, true}) {
        chr_.skips_budget = skips;
        for (const ValidateBudget b : budgets) {
            for (std::uint32_t frame : {0u, 1u, 2u, 7u, 0xffffffffu}) {
                set_frame(frame);
                expect_same(call(true, b), call(false, b), "30");
            }
        }
    }
}

TEST_F(ChrValidatePaceTest, SpendsTheBudgetOnOddFramesAt60) {
    bb::game::set_frame_rate(60);
    for (const bool skips : {false, true}) {
        chr_.skips_budget = skips;
        for (const ValidateBudget b : budgets) {
            for (std::uint32_t frame : {0u, 1u, 2u, 7u, 0xfffffffeu, 0xffffffffu}) {
                set_frame(frame);
                const Call hooked = call(true, b);
                ValidateBudget seen = b;
                if (frame % 2 == 1 && b.limit >= 0) {
                    seen.limit = 0;
                }
                Call game = call(false, seen);
                game.budget.limit = b.limit;  // the wrapper leaves the caller's limit alone
                expect_same(hooked, game, frame % 2 ? "60 odd" : "60 even");
            }
        }
    }
}

// The gate's other waits stay as they are on frames of either parity: a null character, a
// behaviour slot still loading (states 1 and 2), and an object at +0x288 whose +0xd8 isn't 6
// unless bit 0x20 of +0x1e1 is set.
TEST_F(ChrValidatePaceTest, KeepsTheGatesOtherWaits) {
    std::array<std::byte, 0x100> other{};
    for (unsigned fps : {30u, 60u}) {
        bb::game::set_frame_rate(fps);
        for (std::uint32_t frame : {4u, 5u}) {
            set_frame(frame);
            for (const bool skips : {false, true}) {
                chr_.skips_budget = skips;
                for (const ValidateBudget b : {ValidateBudget{1, 0}, ValidateBudget{-1, 0}}) {
                    const auto both = [&](const char* what) {
                        const Call hooked = call(true, b);
                        ValidateBudget seen = b;
                        if (fps == 60 && frame % 2 == 1 && b.limit >= 0) {
                            seen.limit = 0;
                        }
                        Call game = call(false, seen);
                        game.budget.limit = b.limit;
                        expect_same(hooked, game, what);
                        return hooked.setups;
                    };
                    null_chr_ = true;
                    EXPECT_EQ(both("null"), 0);
                    null_chr_ = false;
                    for (std::uint32_t id : {1u, 2u, 5u}) {
                        for (std::uint32_t state : {0u, 1u, 2u, 3u}) {
                            set_slot(id, state);
                            const int setups = both("slot");
                            if (!skips && (state == 1 || state == 2)) {
                                EXPECT_EQ(setups, 0) << id << " " << state;
                            }
                        }
                    }
                    set_slot(0, 0);
                    field<void*>(chr_, 0x288) = other.data();
                    other[0xd8] = std::byte{5};
                    for (std::uint8_t flags : {0x00, 0x20}) {
                        field<std::uint8_t>(chr_, 0x1e1) = flags;
                        const int setups = both("+0x288");
                        if (flags == 0) {
                            EXPECT_EQ(setups, 0);
                        }
                    }
                    field<std::uint8_t>(chr_, 0x1e1) = 0;
                    other[0xd8] = std::byte{6};
                    both("+0x288 done");
                    field<void*>(chr_, 0x288) = nullptr;
                }
            }
        }
    }
}

// Ten characters wait for their setup, each frame offering each one the frame's budget, as
// chr_frame_update does: at 30 one is set up a frame, at 60 one every second frame, so all are
// set up after 10 frames at 30 and 20 at 60, the same game time.
TEST_F(ChrValidatePaceTest, SetsUpAsManyAGameSecondAt60) {
    for (unsigned fps : {30u, 60u}) {
        bb::game::set_frame_rate(fps);
        bb::hook::apply(bb::game::image(), hooked_);
        std::array<int, 10> setups{};
        std::vector<int> per_frame;
        for (std::uint32_t frame = 100; per_frame.size() < 40; ++frame) {
            set_frame(frame);
            ValidateBudget budget{1, 0};
            int done = 0;
            for (int& s : setups) {
                if (s != 0) {
                    continue;
                }
                chr_.setups = 0;
                if (bb::game::fn<bool(ValidateBudget*, void*)>(chr_validate)(&budget, &chr_)) {
                    s = chr_.setups;
                    ++done;
                }
            }
            per_frame.push_back(done);
        }
        bb::hook::remove(bb::game::image(), hooked_);
        const unsigned k = fps / 30;
        for (std::size_t i = 0; i < per_frame.size(); ++i) {
            const int expected = i < 10 * k && i % k == 0 ? 1 : 0;
            EXPECT_EQ(per_frame[i], expected) << fps << " frame " << i;
        }
        for (int s : setups) {
            EXPECT_EQ(s, 1);
        }
    }
}

// While WorldChrMan's counter stands still (the load steps run chr_frame_update without its frame
// update), the game's gate runs as it does, once a frame, whatever the counter's parity: an odd
// counter that stands still does not stop the setups.
TEST_F(ChrValidatePaceTest, LetsThroughWhileTheWorldStandsStill) {
    bb::game::set_frame_rate(60);
    set_frame(7);  // the frame it changed: paced
    EXPECT_EQ(call(true, {1, 0}).setups, 0);
    for (int i = 0; i < 6; ++i) {  // then each frame is let through
        bb::game::set_frame_count(bb::game::frame_count() + 1);
        const Call hooked = call(true, {1, 0});
        expect_same(hooked, call(false, {1, 0}), "still");
        EXPECT_EQ(hooked.setups, 1) << i;
    }
    set_frame(8);  // the world goes on: even, then odd
    EXPECT_EQ(call(true, {1, 0}).setups, 1);
    set_frame(9);
    EXPECT_EQ(call(true, {1, 0}).setups, 0);
}

// The world's frames as WorldFrames tells them.
TEST(WorldFrames, PacesByTheCounterWhileItAdvances) {
    bb::game::WorldFrames frames;
    EXPECT_EQ(frames.frame(5, 100), 5u);
    EXPECT_EQ(frames.frame(5, 100), 5u);  // several gate calls in a frame
    EXPECT_EQ(frames.frame(6, 101), 6u);  // a new value each frame while the world goes on
    EXPECT_EQ(frames.frame(7, 102), 7u);
    EXPECT_EQ(frames.frame(7, 103), 0u);  // stands still: let through
    EXPECT_EQ(frames.frame(7, 200), 0u);
    EXPECT_EQ(frames.frame(8, 201), 8u);
    EXPECT_EQ(frames.frame(9, 0xffffffffu), 9u);
    EXPECT_EQ(frames.frame(10, 0), 10u);  // the main loop's count wraps
    EXPECT_EQ(frames.frame(10, 1), 0u);
}

// The wrapper without the game: the budget the original sees and what reaches the caller.
ValidateBudget seen_budget{};
bool stand_in(ValidateBudget* budget, void*) {
    seen_budget = *budget;
    if (budget->limit >= 0 && budget->limit <= budget->count) {
        return false;
    }
    ++budget->count;
    return true;
}

TEST(ChrValidatePaced, PassesTheBudgetThroughOnDueFrames) {
    for (std::int32_t scale : {1, 2}) {
        for (std::uint32_t frame : {0u, 2u, 4u, 1u, 3u}) {
            if (scale == 2 && frame % 2 == 1) {
                continue;
            }
            ValidateBudget b{1, 0};
            EXPECT_TRUE(bb::game::chr_validate_paced(&stand_in, &b, nullptr, frame, scale));
            EXPECT_EQ(seen_budget.limit, 1);
            EXPECT_EQ(b.count, 1);
        }
    }
}

TEST(ChrValidatePaced, HandsASpentBudgetOnOtherFrames) {
    ValidateBudget b{2, 1};
    EXPECT_FALSE(bb::game::chr_validate_paced(&stand_in, &b, nullptr, 3, 2));
    EXPECT_EQ(seen_budget.limit, 0);
    EXPECT_EQ(seen_budget.count, 1);
    EXPECT_EQ(b.limit, 2);
    EXPECT_EQ(b.count, 1);
    ValidateBudget unlimited{-1, 4};
    EXPECT_TRUE(bb::game::chr_validate_paced(&stand_in, &unlimited, nullptr, 3, 2));
    EXPECT_EQ(seen_budget.limit, -1);
    EXPECT_EQ(unlimited.count, 5);
}

}  // namespace
