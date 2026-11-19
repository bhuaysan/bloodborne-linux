// Differential test of the loading page's hold patch in src/game/load/load_hold.cpp. The patch
// sits in a function too large to run whole with fakes, so the test runs only the patched bytes: a
// thunk jumps to their first byte with r12 = a fake MoveMapStep, and a landing patched in right
// behind them stores xmm0 (the hold the game then writes to +0x278) and returns. With the game's
// hold the patch gives the game's xmm0 bit for bit; with another hold, that hold.
#include "game/load/load_hold.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <optional>
#include <stdexcept>

#include <gtest/gtest.h>

#include "game/image.hpp"
#include "harness/eboot.hpp"
#include "hook/hook.hpp"

// bb_lh_run_bytes(entry, object) saves the callee-saved registers and the stack pointer, sets r12
// to `object`, sets every bit of xmm0 (so a load that keeps its upper lanes shows) and jumps to
// `entry`. bb_lh_land, patched in behind the bytes under test, stores all 16 bytes of xmm0 and
// returns from bb_lh_run_bytes.
extern "C" {
void bb_lh_run_bytes(std::uint64_t entry, void* object);
void bb_lh_land();
std::uint64_t bb_lh_saved_rsp = 0;
std::uint64_t bb_lh_land_continue = 0;  // filled by apply, unused
alignas(16) std::uint32_t bb_lh_xmm0[4] = {};
}

asm(R"(
    .pushsection .text
    .intel_syntax noprefix
    .globl bb_lh_run_bytes
    .type bb_lh_run_bytes, @function
bb_lh_run_bytes:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    mov qword ptr [rip + bb_lh_saved_rsp], rsp
    mov r12, rsi
    vpcmpeqd xmm0, xmm0, xmm0
    jmp rdi
    .size bb_lh_run_bytes, . - bb_lh_run_bytes

    .globl bb_lh_land
    .type bb_lh_land, @function
bb_lh_land:
    vmovups xmmword ptr [rip + bb_lh_xmm0], xmm0
    mov rsp, qword ptr [rip + bb_lh_saved_rsp]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret
    .size bb_lh_land, . - bb_lh_land
    .att_syntax prefix
    .popsection
)");

namespace {

using bb::hook::Replacement;
using bb::test::EbootTest;

std::uint32_t float_bits(float f) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof bits);
    return bits;
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
class Hooked {
public:
    explicit Hooked(const Replacement& hook) : hooks_{hook} {
        bb::hook::apply(bb::game::image(), hooks_);
    }
    ~Hooked() { bb::hook::remove(bb::game::image(), hooks_); }
    Hooked(const Hooked&) = delete;
    Hooked& operator=(const Hooked&) = delete;

private:
    std::array<Replacement, 1> hooks_;
};

// Sets the hold for one test and puts back the game's, also when the test fails early.
class HoldGuard {
public:
    explicit HoldGuard(float seconds) { bb::game::set_load_hold(seconds); }
    ~HoldGuard() { bb::game::set_load_hold(bb::game::game_load_hold); }
    HoldGuard(const HoldGuard&) = delete;
    HoldGuard& operator=(const HoldGuard&) = delete;
};

// 0x01938b72: vmovss [r12+0x278], xmm0; mov dword [r12+0x27c], 0x41700000 (whole instructions of
// at least hook::patch_size bytes).
constexpr std::array<std::uint8_t, 22> land_bytes = {
    0xc4, 0xc1, 0x7a, 0x11, 0x84, 0x24, 0x78, 0x02, 0x00, 0x00, 0x41,
    0xc7, 0x84, 0x24, 0x7c, 0x02, 0x00, 0x00, 0x00, 0x00, 0x70, 0x41};

// xmm0 at 0x01938b72 for a step whose +0xe4 is `flag`, with the patch applied when `patched`.
// Lanes 1 to 3 are 0 with the game's bytes and with the stub; the test fails here otherwise.
std::uint32_t hold_bits(bool patched, std::uint8_t flag) {
    alignas(16) std::array<std::byte, 0x300> step{};
    step[0xe4] = std::byte{flag};
    const Hooked landed(Replacement{bb::game::load_hold_continue,
                                    reinterpret_cast<void*>(&bb_lh_land), "bb_lh_land", land_bytes,
                                    bb::game::move_map_load_start, &bb_lh_land_continue});
    std::optional<Hooked> patch;
    if (patched) {
        patch.emplace(registered(bb::game::load_hold_load));
    }
    std::fill(std::begin(bb_lh_xmm0), std::end(bb_lh_xmm0), 0xdeadbeefu);
    bb_lh_run_bytes(bb::game::image().runtime(bb::game::load_hold_load), step.data());
    EXPECT_EQ(bb_lh_xmm0[1] | bb_lh_xmm0[2] | bb_lh_xmm0[3], 0u);
    return bb_lh_xmm0[0];
}

using LoadHoldTest = EbootTest;

TEST_F(LoadHoldTest, PatchIsRegisteredInsideItsFunction) {
    EXPECT_EQ(registered(bb::game::load_hold_load).function, bb::game::move_map_load_start);
    const Hooked patch(registered(bb::game::load_hold_load));
    EXPECT_EQ(bb_load_hold_continue, bb::game::image().runtime(bb::game::load_hold_continue));
}

TEST_F(LoadHoldTest, GameHoldIsTheGames) {
    for (const std::uint8_t flag : {0, 1, 0x80, 0xff}) {
        const std::uint32_t game = hold_bits(false, flag);
        EXPECT_EQ(game, flag != 0 ? float_bits(12.0f) : 0u) << int{flag};
        EXPECT_EQ(hold_bits(true, flag), game) << int{flag};
    }
}

TEST_F(LoadHoldTest, ShorterHoldOnlyForLoadsWithAHold) {
    for (const float seconds : {0.0f, 1.0f, 3.5f}) {
        const HoldGuard hold(seconds);
        EXPECT_EQ(hold_bits(true, 1), float_bits(seconds)) << seconds;
        EXPECT_EQ(hold_bits(true, 0), 0u) << seconds;
    }
}

TEST(LoadHold, RefusesHoldsOutsideZeroToTwelve) {
    EXPECT_THROW(bb::game::set_load_hold(-0.5f), std::invalid_argument);
    EXPECT_THROW(bb::game::set_load_hold(12.5f), std::invalid_argument);
    EXPECT_THROW(bb::game::set_load_hold(std::nanf("")), std::invalid_argument);
    EXPECT_EQ(bb::game::load_hold(), bb::game::game_load_hold);
    bb::game::set_load_hold(0.0f);
    EXPECT_EQ(bb::game::load_hold(), 0.0f);
    bb::game::set_load_hold(bb::game::game_load_hold);
}

}  // namespace
