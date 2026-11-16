// Differential tests of the character due check in src/game/chr/: the stub that replaces the game's
// bytes at 0x01917909 leaves at 30 FPS the same registers and frame slot as the game's bytes, and
// at 60 FPS what the game's bytes leave for twice the interval, with the interval itself kept.
#include "game/chr/update_interval.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include "game/frame/frame.hpp"
#include "game/image.hpp"
#include "harness/eboot.hpp"
#include "hook/hook.hpp"

namespace {

using bb::hook::Replacement;
using bb::test::EbootTest;

constexpr std::uint64_t world_chr_update = 0x019173b0;

// One run of the check from 0x01917909 to the instruction behind it, as the driver below sees it.
struct DueRun {
    std::uint32_t interval;  // ecx
    std::int32_t index;      // r13d
    std::uint64_t world;     // rdi
    std::uint64_t frame;     // rbp
    // 1: due (0x0191792d, the check stored the interval to [rbp-0xc8]); 2: not due (0x01917a1b,
    // whose instruction, replaced by the capture, stores ecx there)
    std::uint32_t exit;
    std::uint32_t eax, ecx, edx, esi;
    std::uint32_t xmm0;      // its low float's bits
    std::uint32_t slot;      // [rbp-0xc8]
    std::uint32_t pad;       // so that memcmp compares no padding
};
static_assert(offsetof(DueRun, exit) == 24 && offsetof(DueRun, slot) == 48 &&
              sizeof(DueRun) == 56);

}  // namespace

// The driver enters the check with the run's registers and frame, and the captures, patched at the
// two instructions behind it, record the state there and return from the driver.
extern "C" {
std::uint64_t chr_due_test_entry = 0;
std::uint64_t chr_due_test_rsp = 0;
void* chr_due_test_run = nullptr;  // the DueRun
void chr_due_test_enter(void* run);
void chr_due_test_due();
void chr_due_test_not_due();
}
asm(R"(
    .pushsection .text
    .intel_syntax noprefix
    .globl chr_due_test_enter
chr_due_test_enter:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    mov qword ptr [rip + chr_due_test_rsp], rsp
    mov qword ptr [rip + chr_due_test_run], rdi
    mov ecx, dword ptr [rdi]
    mov r13d, dword ptr [rdi + 4]
    mov rbp, qword ptr [rdi + 16]
    mov rdi, qword ptr [rdi + 8]
    vxorps xmm0, xmm0, xmm0
    mov eax, 0x11111111
    mov edx, 0x22222222
    mov esi, 0x33333333
    jmp qword ptr [rip + chr_due_test_entry]
    .globl chr_due_test_due
chr_due_test_due:
    mov r8, qword ptr [rip + chr_due_test_run]
    mov dword ptr [r8 + 24], 1
    jmp 1f
    .globl chr_due_test_not_due
chr_due_test_not_due:
    mov r8, qword ptr [rip + chr_due_test_run]
    mov dword ptr [r8 + 24], 2
1:
    mov dword ptr [r8 + 28], eax
    mov dword ptr [r8 + 32], ecx
    mov dword ptr [r8 + 36], edx
    mov dword ptr [r8 + 40], esi
    vmovd dword ptr [r8 + 44], xmm0
    mov r9d, dword ptr [rbp - 0xc8]
    mov dword ptr [r8 + 48], r9d
    mov rsp, qword ptr [rip + chr_due_test_rsp]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret
    .att_syntax prefix
    .popsection
)");

namespace {

// vmulss xmm0, xmm0, [rbp-0xb0]; vmovss [r12+8], xmm0
constexpr std::array<std::uint8_t, 15> due_bytes = {0xc5, 0xfa, 0x59, 0x85, 0x50, 0xff, 0xff, 0xff,
                                                    0xc4, 0xc1, 0x7a, 0x11, 0x44, 0x24, 0x08};
// mov [rbp-0xc8], ecx; mov rax, [rdi+0x61d0]; test rax, rax
constexpr std::array<std::uint8_t, 16> not_due_bytes = {0x89, 0x8d, 0x38, 0xff, 0xff, 0xff,
                                                        0x48, 0x8b, 0x87, 0xd0, 0x61, 0x00,
                                                        0x00, 0x48, 0x85, 0xc0};

// The registered patch of the check (BB_PATCH) and wrapper of the update round (BB_WRAP).
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

class ChrUpdateIntervalTest : public EbootTest {
protected:
    void SetUp() override {
        EbootTest::SetUp();
        if (IsSkipped()) {
            return;
        }
        chr_due_test_entry = bb::game::image().runtime(bb::game::chr_due_check);
        captures_.emplace(std::array{
            Replacement{bb::game::chr_due_continue, reinterpret_cast<void*>(&chr_due_test_due),
                        "due", due_bytes, world_chr_update},
            Replacement{bb::game::chr_not_due_continue,
                        reinterpret_cast<void*>(&chr_due_test_not_due), "not_due",
                        not_due_bytes, world_chr_update}});
    }
    void TearDown() override {
        captures_.reset();
        bb_chr_due_scale = 1;  // no image() here: a skipped test has none
    }

    // Runs the check in memory now for a character at `index` with interval `interval` in frame
    // `counter`.
    DueRun run(std::uint32_t interval, std::int32_t index, std::uint32_t counter) {
        std::memcpy(world_.data() + 0xa68, &counter, sizeof counter);
        frame_.fill(std::byte{0x5a});
        DueRun out{};
        out.interval = interval;
        out.index = index;
        out.world = reinterpret_cast<std::uint64_t>(world_.data());
        out.frame = reinterpret_cast<std::uint64_t>(frame_.data() + 0x100);
        chr_due_test_enter(&out);
        return out;
    }

    std::array<std::byte, 0x200> frame_{};
    std::vector<std::byte> world_ = std::vector<std::byte>(0x1000);
    std::optional<Hooked<2>> captures_;
};

constexpr std::uint32_t intervals[] = {1, 2, 3, 5, 30};

TEST_F(ChrUpdateIntervalTest, IsTheGamesCheckAt30) {
    const Hooked<1> stub(std::array{registered(bb::game::chr_due_check)});
    for (std::uint32_t u : intervals) {
        for (std::int32_t index = 0; index < 40; ++index) {
            for (std::uint32_t counter : {0u, 1u, 4u, 29u, 30u, 61u, 899u, 0xffffffffu}) {
                bb::game::set_chr_due_check(1);
                const DueRun stubbed = run(u, index, counter);
                // The game's bytes: the stub's patch taken out for the call.
                std::array once{registered(bb::game::chr_due_check)};
                bb::hook::remove(bb::game::image(), once);
                const DueRun game = run(u, index, counter);
                bb::hook::apply(bb::game::image(), once);
                ASSERT_EQ(std::memcmp(&stubbed, &game, sizeof game), 0)
                    << "u " << u << " index " << index << " counter " << counter;
                // Due, the check stored u; not due, the instruction behind it stores ecx.
                ASSERT_EQ(game.exit == 1 ? game.slot : game.ecx, u);
            }
        }
    }
}

TEST_F(ChrUpdateIntervalTest, ChecksTwiceTheIntervalAt60AndKeepsIt) {
    std::array once{registered(bb::game::chr_due_check)};
    for (std::uint32_t u : intervals) {
        for (std::int32_t index = 0; index < 40; ++index) {
            for (std::uint32_t counter = 0; counter < 4 * u + 3; ++counter) {
                const DueRun game = run(2 * u, index, counter);  // the game's bytes, 2u
                bb::hook::apply(bb::game::image(), once);
                bb::game::set_chr_due_check(2);
                const DueRun stubbed = run(u, index, counter);
                bb::hook::remove(bb::game::image(), once);
                ASSERT_EQ(stubbed.exit, game.exit) << u << " " << index << " " << counter;
                EXPECT_EQ(stubbed.eax, game.eax);
                EXPECT_EQ(stubbed.edx, game.edx);
                EXPECT_EQ(stubbed.esi, game.esi);
                EXPECT_EQ(stubbed.xmm0, game.xmm0);  // 2u, times the step at 0x0191792d
                EXPECT_EQ(stubbed.ecx, u);  // stored to [rbp-0xc8] behind the check when not due
                if (stubbed.exit == 1) {
                    EXPECT_EQ(stubbed.slot, u);  // the level of detail stays u
                }
            }
        }
    }
}

TEST_F(ChrUpdateIntervalTest, IsDueOnceIn2uFramesAt60) {
    // The game's bytes: once in u frames, so twice as often a game second at 60 FPS.
    for (std::uint32_t u : intervals) {
        unsigned due = 0;
        for (std::uint32_t counter = 100; counter < 100 + 6 * u; ++counter) {
            due += run(u, 7, counter).exit == 1;
        }
        EXPECT_EQ(due, 6u) << "u " << u;
    }
    const Hooked<1> stub(std::array{registered(bb::game::chr_due_check)});
    bb::game::set_chr_due_check(2);
    for (std::uint32_t u : intervals) {
        for (std::int32_t index = 0; index < 40; ++index) {
            unsigned due = 0;
            for (std::uint32_t counter = 100; counter < 100 + 6 * u; ++counter) {
                const DueRun r = run(u, index, counter);
                if (r.exit == 1) {
                    ++due;
                    float steps;
                    std::memcpy(&steps, &r.xmm0, sizeof steps);
                    EXPECT_EQ(steps, static_cast<float>(2 * u));  // 2u steps of 1/60: u of 1/30
                } else {
                    EXPECT_EQ(r.exit, 2u);
                }
            }
            EXPECT_EQ(due, 3u) << "u " << u << " index " << index;
        }
    }
}

// world_chr_update's original, recording the scale the check has when it runs.
std::int32_t scale_seen = 0;
void fake_update(float, void*) {
    scale_seen = bb_chr_due_scale;
}

TEST_F(ChrUpdateIntervalTest, TheWrapperSetsTheScaleForTheFrameRate) {
    const Hooked<2> both(
        std::array{registered(world_chr_update), registered(bb::game::chr_due_check)});
    EXPECT_EQ(registered(bb::game::chr_due_check).function, world_chr_update);
    for (unsigned fps : {60u, 30u}) {
        bb::game::set_frame_rate(fps);
        bb::game::world_chr_update_with(&fake_update, 1.0f / static_cast<float>(fps), nullptr);
        EXPECT_EQ(scale_seen, static_cast<std::int32_t>(fps / 30));
        EXPECT_EQ(bb_chr_due_continue, bb::game::image().runtime(bb::game::chr_due_continue));
        EXPECT_EQ(bb_chr_due_not_due, bb::game::image().runtime(bb::game::chr_not_due_continue));
    }
    bb::game::set_frame_rate(bb::game::default_frame_rate);
}

}  // namespace
