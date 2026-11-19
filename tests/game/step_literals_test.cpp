// Differential tests of the step patches in src/game/frame/step_literals.cpp: each patched game
// function runs whole, once with the game's bytes and once with the patch, from the same state; at
// 30 FPS the harness finds no difference in anything they touch, and at 60 FPS the step they pass
// on is 1/60. Their callees are fakes that record what they get. The effect lights' and the hold's
// patches sit in functions too large to run whole with fakes, so their tests run only the patched
// bytes: a thunk jumps to their first byte, and a landing patched right behind them returns.
#include "game/frame/step_literals.hpp"

#include <algorithm>
#include <array>
#include <iterator>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
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

using bb::game::at;

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

using StepLiteralsTest = EbootTest;

TEST_F(StepLiteralsTest, PatchesAreRegisteredInsideTheirFunctions) {
    EXPECT_EQ(registered(bb::game::light_step_load).function, bb::game::effect_light_gather);
    EXPECT_EQ(registered(bb::game::light_step_empty_load).function,
              bb::game::effect_light_gather);
    EXPECT_EQ(registered(bb::game::hold_count).function, bb::game::effect_object_update);
    EXPECT_EQ(registered(bb::game::sfx_step_load).function, bb::game::sfx_frame_update);
    EXPECT_EQ(registered(bb::game::shop_tab_update).function, bb::game::shop_tab_refresh);
    EXPECT_EQ(registered(bb::game::chr_fade_add).function, bb::game::chr_list_fade_step);
    // apply fills the continuations.
    const Hooked<3> patches(std::array{registered(bb::game::sfx_step_load),
                                       registered(bb::game::shop_tab_update),
                                       registered(bb::game::chr_fade_add)});
    EXPECT_EQ(bb_sfx_step_continue, bb::game::image().runtime(bb::game::sfx_step_continue));
    EXPECT_EQ(bb_shop_tab_continue, bb::game::image().runtime(bb::game::shop_tab_continue));
    EXPECT_EQ(bb_chr_fade_continue, bb::game::image().runtime(bb::game::chr_fade_continue));
    const Hooked<3> more(std::array{registered(bb::game::light_step_load),
                                    registered(bb::game::light_step_empty_load),
                                    registered(bb::game::hold_count)});
    EXPECT_EQ(bb_light_step_continue, bb::game::image().runtime(bb::game::light_step_continue));
    EXPECT_EQ(bb_light_step_target, bb::game::image().runtime(bb::game::light_step_target));
    EXPECT_EQ(bb_light_step_empty_continue,
              bb::game::image().runtime(bb::game::light_step_empty_continue));
    EXPECT_EQ(bb_hold_continue, bb::game::image().runtime(bb::game::hold_continue));
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

// From 0 the fade reaches 1 in a second of calls at either rate: 30 calls at 30, 61 at 60 (an
// ulp short after 60). The kind 7 part gets each value, the kind 3 part none.
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
    // The float sums are exact to the call: 30 steps of 1/30 reach 1, while 60 steps of 1/60 fall
    // short of it by an ulp, so the 61st call clamps.
    EXPECT_EQ(calls_to_full(false), 30);
    {
        const FrameRateGuard rate(60);
        EXPECT_EQ(calls_to_full(true), 61);
    }
    harness().restore();
}

// ---- the patched bytes alone ---------------------------------------------------------------------

}  // namespace

// bb_test_run_bytes(entry, object) saves the callee-saved registers and the stack pointer, sets rax
// and r12 to `object` and jumps to `entry`. bb_test_land, patched in behind the bytes under test,
// stores all 16 bytes of xmm0 and xmm4 (the game reads the step as a vector later) and returns from
// bb_test_run_bytes.
extern "C" {
void bb_test_run_bytes(std::uint64_t entry, void* object);
void bb_test_land();
std::uint64_t bb_test_saved_rsp = 0;
std::uint64_t bb_test_land_continue = 0;  // filled by apply, unused
alignas(16) std::uint32_t bb_test_xmm0[4] = {};
alignas(16) std::uint32_t bb_test_xmm4[4] = {};
}

asm(R"(
    .pushsection .text
    .intel_syntax noprefix
    .globl bb_test_run_bytes
    .type bb_test_run_bytes, @function
bb_test_run_bytes:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    mov qword ptr [rip + bb_test_saved_rsp], rsp
    mov rax, rsi
    mov r12, rsi
    jmp rdi
    .size bb_test_run_bytes, . - bb_test_run_bytes

    .globl bb_test_land
    .type bb_test_land, @function
bb_test_land:
    vmovups xmmword ptr [rip + bb_test_xmm0], xmm0
    vmovups xmmword ptr [rip + bb_test_xmm4], xmm4
    mov rsp, qword ptr [rip + bb_test_saved_rsp]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret
    .size bb_test_land, . - bb_test_land
    .att_syntax prefix
    .popsection
)");

namespace {

// The game's bytes at the landings, whole instructions of at least hook::patch_size bytes.
// 0x02315d79: vmovss xmm13, [0x04929e6c]; xor r13d, r13d; test r14, r14
constexpr std::array<std::uint8_t, 14> light_land_bytes = {
    0xc5, 0x7a, 0x10, 0x2d, 0xeb, 0x40, 0x61, 0x02, 0x45, 0x31, 0xed, 0x4d, 0x85, 0xf6};
// 0x02313dbf: vmovaps [rbp-0x170], xmm0; xor r13d, r13d; xor r14d, r14d
constexpr std::array<std::uint8_t, 14> light_empty_land_bytes = {
    0xc5, 0xf8, 0x29, 0x85, 0x90, 0xfe, 0xff, 0xff, 0x45, 0x31, 0xed, 0x45, 0x31, 0xf6};
// 0x0231752a: mov byte [r12+0x4c], 0; lea rdi, [rbp-0x78]; xor esi, esi; call 0x02bbe728
constexpr std::array<std::uint8_t, 17> hold_land_bytes = {
    0x41, 0xc6, 0x44, 0x24, 0x4c, 0x00, 0x48, 0x8d, 0x7d,
    0x88, 0x31, 0xf6, 0xe8, 0xed, 0x71, 0x8a, 0x00};

Replacement landing(std::uint64_t function, std::uint64_t link,
                    std::span<const std::uint8_t> bytes) {
    return Replacement{link,     reinterpret_cast<void*>(&bb_test_land), "bb_test_land", bytes,
                       function, &bb_test_land_continue};
}

// Runs the bytes from `entry` to the landing at `land` with rax = r12 = object, with the patch at
// `entry` applied when `patched`.
void run_bytes(std::uint64_t function, std::uint64_t entry, std::uint64_t land,
               std::span<const std::uint8_t> land_bytes, bool patched, void* object) {
    const Hooked<1> landed(std::array{landing(function, land, land_bytes)});
    std::optional<Hooked<1>> patch;
    if (patched) {
        patch.emplace(std::array{registered(entry)});
    }
    std::fill(std::begin(bb_test_xmm0), std::end(bb_test_xmm0), 0xdeadbeefu);
    std::fill(std::begin(bb_test_xmm4), std::end(bb_test_xmm4), 0xdeadbeefu);
    bb_test_run_bytes(bb::game::image().runtime(entry), object);
}

// The light step, from a graphics object whose present_interval (+0x100) is `interval`: xmm4 at
// 0x02315d79 for the main path, xmm0 at 0x02313dbf for the empty one. Lanes 1 to 3 are 0 on both
// paths, with the game's bytes and with the stubs; the test fails here otherwise.
std::uint32_t light_step(bool empty, bool patched, std::int32_t interval) {
    alignas(16) std::array<std::byte, 0x200> graphics{};
    at<std::int32_t>(graphics.data(), 0x100) = interval;
    if (empty) {
        run_bytes(bb::game::effect_light_gather, bb::game::light_step_empty_load,
                  bb::game::light_step_empty_continue, light_empty_land_bytes, patched,
                  graphics.data());
        EXPECT_EQ(bb_test_xmm0[1] | bb_test_xmm0[2] | bb_test_xmm0[3], 0u);
        return bb_test_xmm0[0];
    }
    run_bytes(bb::game::effect_light_gather, bb::game::light_step_load,
              bb::game::light_step_target, light_land_bytes, patched, graphics.data());
    EXPECT_EQ(bb_test_xmm4[1] | bb_test_xmm4[2] | bb_test_xmm4[3], 0u);
    return bb_test_xmm4[0];
}

TEST_F(StepLiteralsTest, LightStepIsTheGamesAt30) {
    for (const bool empty : {false, true}) {
        for (const std::int32_t interval : {0, 1, 2, -1}) {
            const std::uint32_t game = light_step(empty, false, interval);
            EXPECT_EQ(game, interval == 1 ? 0x3c888889u : game_step_bits)
                << "empty " << empty << " interval " << interval;
            EXPECT_EQ(light_step(empty, true, interval), game)
                << "empty " << empty << " interval " << interval;
        }
    }
}

TEST_F(StepLiteralsTest, LightStepIsHalvedAt60) {
    const FrameRateGuard rate(60);
    for (const bool empty : {false, true}) {
        for (const std::int32_t interval : {0, 1, 2}) {
            EXPECT_EQ(light_step(empty, true, interval),
                      float_bits(interval == 1 ? 1.0f / 120.0f : 1.0f / 60.0f))
                << "empty " << empty << " interval " << interval;
        }
    }
}

struct HoldState {
    std::uint8_t active;  // +0x4d
    std::int32_t count;   // +0x50
    std::uint32_t phase;  // bb_hold_phase
    bool operator==(const HoldState&) const = default;
};

HoldState hold_step(bool patched, HoldState in) {
    alignas(16) std::array<std::byte, 0x60> object{};
    at<std::uint8_t>(object.data(), 0x4c) = 0x77;  // the landing skips the game's clear of +0x4c
    at<std::uint8_t>(object.data(), 0x4d) = in.active;
    at<std::int32_t>(object.data(), 0x50) = in.count;
    bb_hold_phase = in.phase;
    run_bytes(bb::game::effect_object_update, bb::game::hold_count, bb::game::hold_continue,
              hold_land_bytes, patched, object.data());
    EXPECT_EQ(at<std::uint8_t>(object.data(), 0x4c), 0x77);
    return {at<std::uint8_t>(object.data(), 0x4d), at<std::int32_t>(object.data(), 0x50),
            bb_hold_phase};
}

TEST_F(StepLiteralsTest, HoldCountIsTheGamesAt30) {
    for (const HoldState in : {HoldState{0, 5, 0}, HoldState{1, 5, 0}, HoldState{1, 1, 0},
                               HoldState{1, 0, 0}, HoldState{2, 60, 0}, HoldState{0, 0, 0}}) {
        const HoldState game = hold_step(false, in);
        EXPECT_EQ(hold_step(true, in), game) << int{in.active} << " " << in.count;
    }
    bb_hold_phase = 0;
}

// A hold of 60 lasts 60 counts and one call that clears it: 61 calls at 30 with the game's bytes
// and with the patch, 121 at 60, and the next hold starts at phase 0.
TEST_F(StepLiteralsTest, HoldLastsTwoSecondsAt60) {
    const auto calls = [](bool patched) {
        HoldState s{1, 60, 0};
        int n = 0;
        while (s.active != 0 && n < 1000) {
            s = hold_step(patched, s);
            ++n;
        }
        EXPECT_EQ(s.phase, 0u);
        return n;
    };
    EXPECT_EQ(calls(false), 61);
    EXPECT_EQ(calls(true), 61);
    {
        const FrameRateGuard rate(60);
        EXPECT_EQ(calls(true), 121);
        // A hold armed while the phase is 1 is one call short.
        EXPECT_EQ(hold_step(true, {1, 3, 1}), (HoldState{1, 2, 0}));
    }
    bb_hold_phase = 0;
}

}  // namespace
