// Differential tests of the step patches in src/game/frame/step_literals.cpp: each patched game
// function runs whole, once with the game's bytes and once with the patch, from the same state; at
// 30 FPS the harness finds no difference in anything they touch, and at 60 FPS the step they pass
// on is 1/60. Their callees are fakes that record what they get.
#include "game/frame/step_literals.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
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
using bb::hook::Replacement;
using bb::test::EbootTest;

template <class T>
T& at(void* object, std::size_t offset) {
    return *reinterpret_cast<T*>(static_cast<std::byte*>(object) + offset);
}

std::uint32_t float_bits(float f) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof bits);
    return bits;
}

constexpr std::uint32_t game_step_bits = 0x3d088889;  // the game's 1/30

// The calls of the fakes, in the arena, so the harness compares them between the runs.
struct Call {
    std::uint64_t self;
    std::uint64_t arg;
    std::uint32_t step_bits;
    std::uint32_t pad;
};
struct Log {
    std::uint32_t count;
    std::uint32_t pad;
    Call calls[8];
};
Log* log_ = nullptr;

void record(void* self, std::uint64_t arg, float step) {
    ASSERT_LT(log_->count, std::size(log_->calls));
    log_->calls[log_->count++] = {reinterpret_cast<std::uint64_t>(self), arg, float_bits(step), 0};
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

// Hooks `hooks` for the lifetime of the object.
template <std::size_t N>
class Hooked {
public:
    explicit Hooked(const std::array<Replacement, N>& hooks) : hooks_(hooks) {
        bb::hook::apply(bb::game::image(), hooks_);
    }
    ~Hooked() { bb::hook::remove(bb::game::image(), hooks_); }
    Hooked(const Hooked&) = delete;
    Hooked& operator=(const Hooked&) = delete;

private:
    std::array<Replacement, N> hooks_;
};

// Sets the frame rate for one test and puts back the default, 30, also when the test fails early.
class FrameRateGuard {
public:
    explicit FrameRateGuard(unsigned fps) { bb::game::set_frame_rate(fps); }
    ~FrameRateGuard() { bb::game::set_frame_rate(bb::game::default_frame_rate); }
    FrameRateGuard(const FrameRateGuard&) = delete;
    FrameRateGuard& operator=(const FrameRateGuard&) = delete;
};

// The game's function at `Function`, called with the patch at `Patch` applied for the call: the
// "replacement" of a comparison.
template <std::uint64_t Function, std::uint64_t Patch, class R, class... A>
R call_patched(A... args) {
    const Hooked<1> patch(std::array{registered(Patch)});
    return bb::game::fn<R(A...)>(Function)(args...);
}

class StepLiteralsTest : public EbootTest {
protected:
    void SetUp() override {
        EbootTest::SetUp();
        if (IsSkipped()) {
            return;
        }
        bb::game::set_step_literal_targets();
    }
};

TEST_F(StepLiteralsTest, PatchesAreRegisteredInsideTheirFunctions) {
    EXPECT_EQ(registered(bb::game::sfx_step_load).function, bb::game::sfx_frame_update);
    EXPECT_EQ(registered(bb::game::shop_tab_update).function, bb::game::shop_tab_refresh);
    EXPECT_EQ(registered(bb::game::chr_fade_add).function, bb::game::chr_list_fade_step);
    EXPECT_EQ(bb_sfx_step_continue, bb::game::image().runtime(bb::game::sfx_step_continue));
    EXPECT_EQ(bb_shop_tab_continue, bb::game::image().runtime(bb::game::shop_tab_continue));
    EXPECT_EQ(bb_chr_fade_continue, bb::game::image().runtime(bb::game::chr_fade_continue));
}

// ---- the effects --------------------------------------------------------------------------------

void fake_sfx_step(void* self, float step) {
    record(self, at<std::uint8_t>(self, 0x20), step);
}

// SprjSfx with an empty effect list, no pending work (+0x38) and no RendMan step (+0x80), so the
// function only steps the effect system at +0x70.
std::tuple<void*, void*> build_sfx(Arena& a, std::int32_t skip, std::uint8_t byte_98) {
    log_ = a.zeroed<Log>();
    auto* sfx = static_cast<std::byte*>(a.alloc(0xa0));
    auto** head = a.zeroed<void*>(4);
    head[0] = head;  // the list's sentinel, alone
    head[1] = head;
    at<void*>(sfx, 0x10) = head;
    auto** vtable = a.zeroed<void*>(16);
    vtable[0x40 / 8] = reinterpret_cast<void*>(&fake_sfx_step);
    auto** system = a.zeroed<void*>(8);
    system[0] = vtable;
    at<void*>(sfx, 0x70) = system;
    at<std::int32_t>(sfx, 0x8c) = skip;
    at<std::uint8_t>(sfx, 0x98) = byte_98;
    return {sfx, a.alloc(0x40)};
}

using SfxUpdate = void(void*, void*);

TEST_F(StepLiteralsTest, SfxStepIsTheGamesAt30) {
    for (const std::int32_t skip : {0, 1, -1}) {
        const Result r = harness().compare_functions<SfxUpdate>(
            bb::game::fn<SfxUpdate>(bb::game::sfx_frame_update),
            &call_patched<bb::game::sfx_frame_update, bb::game::sfx_step_load, void, void*, void*>,
            [&](Arena& a) { return build_sfx(a, skip, 0x5a); });
        EXPECT_TRUE(r) << "skip " << skip << ": " << r.report();
        ASSERT_EQ(log_->count, 1u);
        EXPECT_EQ(log_->calls[0].step_bits, skip == 0 ? game_step_bits : 0u);
        EXPECT_EQ(log_->calls[0].arg, 0x5au);
    }
    harness().restore();
}

TEST_F(StepLiteralsTest, SfxStepIsTheFrameStepAt60) {
    const FrameRateGuard rate(60);
    const Hooked<1> patch(std::array{registered(bb::game::sfx_step_load)});
    for (const std::int32_t skip : {0, 1}) {
        harness().restore();
        auto [sfx, data] = build_sfx(harness().arena(), skip, 1);
        bb::game::fn<SfxUpdate>(bb::game::sfx_frame_update)(sfx, data);
        ASSERT_EQ(log_->count, 1u);
        EXPECT_EQ(log_->calls[0].step_bits, skip == 0 ? float_bits(1.0f / 60.0f) : 0u);
        EXPECT_EQ(at<std::int32_t>(sfx, 0x8c), 0);  // the skip is used up
    }
    harness().restore();
}

// ---- the shop's tab highlight -------------------------------------------------------------------

constexpr std::uint64_t layout_find = 0x00ce5990;  // (layout, name) -> instance or null
constexpr std::uint64_t shop_tab_mode = 0x05561be8;
void* found_ = nullptr;  // what fake_find returns

void* fake_layout(void* self) {
    return static_cast<std::byte*>(self) + 0x100;
}
void* fake_find(void* layout, const char16_t* name) {
    record(layout, reinterpret_cast<std::uint64_t>(name), 0.0f);
    return found_;
}
void fake_instance_update(void* self, float step) {
    record(self, 0, step);
}

// The shop menu with neither +0x1e0 nor +0x1e1 set, so the function returns behind the update.
std::tuple<void*> build_shop(Arena& a, bool instance, std::uint8_t mode) {
    log_ = a.zeroed<Log>();
    auto** vtable = a.zeroed<void*>(32);
    vtable[0x80 / 8] = reinterpret_cast<void*>(&fake_layout);
    auto* shop = static_cast<std::byte*>(a.alloc(0x300));
    at<void*>(shop, 0) = vtable;
    found_ = nullptr;
    if (instance) {
        auto** ivtable = a.zeroed<void*>(32);
        ivtable[0x88 / 8] = reinterpret_cast<void*>(&fake_instance_update);
        auto** inst = a.zeroed<void*>(8);
        inst[0] = ivtable;
        found_ = inst;
    }
    bb::game::global<std::uint8_t>(shop_tab_mode) = mode;
    return {shop};
}

using ShopTabRefresh = void(void*);

TEST_F(StepLiteralsTest, ShopTabUpdateIsTheGamesAt30) {
    const Hooked<1> find(std::array{
        Replacement{layout_find, reinterpret_cast<void*>(&fake_find), "fake_find"}});
    for (const bool instance : {true, false}) {
        for (const std::uint8_t mode : {std::uint8_t{2}, std::uint8_t{0}}) {
            const Result r = harness().compare_functions<ShopTabRefresh>(
                bb::game::fn<ShopTabRefresh>(bb::game::shop_tab_refresh),
                &call_patched<bb::game::shop_tab_refresh, bb::game::shop_tab_update, void, void*>,
                [&](Arena& a) { return build_shop(a, instance, mode); });
            EXPECT_TRUE(r) << "instance " << instance << " mode " << int{mode} << ": "
                           << r.report();
            ASSERT_EQ(log_->count, instance ? 2u : 1u);
            if (instance) {
                EXPECT_EQ(log_->calls[1].self, reinterpret_cast<std::uint64_t>(found_));
                EXPECT_EQ(log_->calls[1].step_bits, game_step_bits);
            }
        }
    }
    harness().restore();
}

TEST_F(StepLiteralsTest, ShopTabUpdateIsTheFrameStepAt60) {
    const FrameRateGuard rate(60);
    const Hooked<2> hooks(std::array{
        Replacement{layout_find, reinterpret_cast<void*>(&fake_find), "fake_find"},
        registered(bb::game::shop_tab_update)});
    harness().restore();
    auto [shop] = build_shop(harness().arena(), true, 2);
    bb::game::fn<ShopTabRefresh>(bb::game::shop_tab_refresh)(shop);
    ASSERT_EQ(log_->count, 2u);
    EXPECT_EQ(log_->calls[1].self, reinterpret_cast<std::uint64_t>(found_));
    EXPECT_EQ(log_->calls[1].step_bits, float_bits(1.0f / 60.0f));
    harness().restore();
}

// ---- the character fade -------------------------------------------------------------------------

constexpr std::uint64_t chr_physics_singleton = 0x0553d700;
std::byte* parts_[8] = {};  // the parts build_fade made, by table slot

std::int32_t fake_kind(void* part) {
    return at<std::int32_t>(part, 0x10);
}

using FadeStep = void(void*, void*, void*, float);

// The fade's owner with `parts` parts on its list (kinds 7, 3, 7, ...), and the singleton's part
// table they index. A blend argument of 0 returns behind the fade and the parts.
std::tuple<void*, void*, void*, float> build_fade(Arena& a, float fade, std::int32_t parts) {
    log_ = a.zeroed<Log>();
    auto* owner = static_cast<std::byte*>(a.alloc(0x200));
    at<float>(owner, 0x1c0) = fade;
    auto* list = static_cast<std::byte*>(a.alloc(0x40));
    at<void*>(owner, 0x38) = list;
    at<std::int32_t>(list, 0x38) = parts;
    auto* index = a.zeroed<std::int32_t>(8);
    at<void*>(list, 0x30) = index;
    auto** vtable = a.zeroed<void*>(8);
    vtable[0x28 / 8] = reinterpret_cast<void*>(&fake_kind);
    auto* table = static_cast<std::byte*>(a.alloc(8 * 0x38));
    for (std::int32_t i = 0; i < parts; ++i) {
        index[i] = parts - 1 - i;
        auto* part = static_cast<std::byte*>(a.alloc(0x190));
        parts_[i] = part;
        at<void*>(part, 0) = vtable;
        at<std::int32_t>(part, 0x10) = i % 2 == 0 ? 7 : 3;
        at<void*>(table, static_cast<std::size_t>(i) * 0x38 + 8) = part;
    }
    auto* holder = static_cast<std::byte*>(a.alloc(0x200));
    at<void*>(holder, 0x128) = table;
    auto* physics = static_cast<std::byte*>(a.alloc(0x40));
    at<void*>(physics, 8) = holder;
    auto* singleton = static_cast<std::byte*>(a.alloc(0x40));
    at<void*>(singleton, 0x28) = physics;
    bb::game::global<void*>(chr_physics_singleton) = singleton;
    return {owner, a.alloc(0x40), a.alloc(0x40), 0.0f};
}

TEST_F(StepLiteralsTest, ChrFadeIsTheGamesAt30) {
    for (const float fade : {0.0f, 0.5f, 0.98f, 0.99f, 1.0f, 1.5f, -0.25f}) {
        for (const std::int32_t parts : {0, 1, 4}) {
            const Result r = harness().compare_functions<FadeStep>(
                bb::game::fn<FadeStep>(bb::game::chr_list_fade_step),
                &call_patched<bb::game::chr_list_fade_step, bb::game::chr_fade_add, void, void*,
                              void*, void*, float>,
                [&](Arena& a) { return build_fade(a, fade, parts); });
            EXPECT_TRUE(r) << "fade " << fade << " parts " << parts << ": " << r.report();
        }
    }
    harness().restore();
}

// From 0 the fade reaches 1 in a second of calls at either rate: 30 calls at 30, 60 at 60. The
// kind 7 part gets each value, the kind 3 part none.
TEST_F(StepLiteralsTest, ChrFadeTakesASecondAt60) {
    const auto calls_to_full = [&](bool patched) {
        harness().restore();
        auto [owner, b, c, blend] = build_fade(harness().arena(), 0.0f, 2);
        std::byte* const seven = parts_[0];  // kind 7, reached through index[1]
        std::byte* const three = parts_[1];  // kind 3, through index[0]
        std::optional<Hooked<1>> patch;
        if (patched) {
            patch.emplace(std::array{registered(bb::game::chr_fade_add)});
        }
        int calls = 0;
        float previous = 0.0f;
        while (at<float>(owner, 0x1c0) < 1.0f && calls < 1000) {
            bb::game::fn<FadeStep>(bb::game::chr_list_fade_step)(owner, b, c, blend);
            ++calls;
            const float fade = at<float>(owner, 0x1c0);
            EXPECT_GT(fade, previous);
            EXPECT_EQ(at<float>(seven, 0x140), fade);
            EXPECT_EQ(at<float>(seven, 0x160), fade);
            EXPECT_EQ(at<float>(seven, 0x180), fade);
            EXPECT_EQ(at<float>(three, 0x140), 0.0f);
            previous = fade;
        }
        return calls;
    };
    const int at30 = calls_to_full(false);
    EXPECT_GE(at30, 30);
    EXPECT_LE(at30, 31);  // float sums of 1/30 may fall short of 1 by an ulp
    {
        const FrameRateGuard rate(60);
        const int at60 = calls_to_full(true);
        EXPECT_GE(at60, 60);
        EXPECT_LE(at60, 61);
    }
    harness().restore();
}

}  // namespace
