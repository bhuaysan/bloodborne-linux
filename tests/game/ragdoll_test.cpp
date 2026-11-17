// Differential tests of the ragdoll changes in src/game/chr/ragdoll.cpp: the pose sync
// (0x01515ec0) against the game's function with its four callees faked, and the two patches inside
// the ragdoll update (0x0180df90), entered at their place from an assembly driver and captured at
// the instruction behind them, against the game's bytes.
#include "game/chr/ragdoll.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
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

std::uint32_t bits(float f) {
    std::uint32_t b = 0;
    std::memcpy(&b, &f, sizeof b);
    return b;
}

std::uint64_t u(const void* p) {
    return reinterpret_cast<std::uint64_t>(p);
}

// Sets the frame rate for one test and puts back the default, 30, also when the test fails early.
class FrameRateGuard {
public:
    explicit FrameRateGuard(unsigned fps) { bb::game::set_frame_rate(fps); }
    ~FrameRateGuard() { bb::game::set_frame_rate(bb::game::default_frame_rate); }
    FrameRateGuard(const FrameRateGuard&) = delete;
    FrameRateGuard& operator=(const FrameRateGuard&) = delete;
};

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

Replacement registered(std::uint64_t link) {
    for (const Replacement& r : bb::hook::replacements()) {
        if (r.link_addr == link) {
            return r;
        }
    }
    ADD_FAILURE() << "nothing registered at " << link;
    return {};
}

// ---- the pose sync ------------------------------------------------------------------------------

enum What : std::uint64_t { update = 1, spaced, idle, lock_init };

struct Call {
    std::uint64_t what;
    std::uint64_t args[6];
};

// In the arena, so the harness compares it between the original and the replacement.
struct Log {
    std::uint32_t count;
    Call calls[8];
};
Log* log_ = nullptr;

// What the lock's init changes in the module, to pin down when the sync reads its fields again.
enum class Mutation { none, chr_gone, ragdoll_swapped, body_swapped };
Mutation mutation_ = Mutation::none;
void* module_ = nullptr;
void* other_ = nullptr;

void record(What what, std::uint64_t a = 0, std::uint64_t b = 0, std::uint64_t c = 0,
            std::uint64_t d = 0, std::uint64_t e = 0, std::uint64_t f = 0) {
    ASSERT_LT(log_->count, std::size(log_->calls));
    log_->calls[log_->count++] = {what, {a, b, c, d, e, f}};
}

void fake_update(float step, float not_read, void* ragdoll, void* transform, void* pose,
                 std::uint32_t flag) {
    record(update, bits(step), bits(not_read), u(ragdoll), u(transform), u(pose), flag);
}
void fake_spaced(float step, float spacing, void* ragdoll, void* transform, void* pose,
                 std::uint32_t flag) {
    record(spaced, bits(step), bits(spacing), u(ragdoll), u(transform), u(pose), flag);
}
// The game's sync leaves rcx unset for this one, so the fake takes no fourth integer.
void fake_idle(float step, float not_read, void* ragdoll, void* transform, void* pose) {
    record(idle, bits(step), bits(not_read), u(ragdoll), u(transform), u(pose));
}
void fake_lock_init(void* lock) {
    record(lock_init, u(lock));
    switch (mutation_) {
        case Mutation::none:
            break;
        case Mutation::chr_gone:
            at<void*>(module_, 0x08) = nullptr;
            break;
        case Mutation::ragdoll_swapped:
            at<void*>(module_, 0x20) = other_;
            break;
        case Mutation::body_swapped:  // the character's +0x3b0 +0x68, read again after the store
            at<void*>(at<void*>(at<void*>(module_, 0x08), 0x3b0), 0x68) = other_;
            break;
    }
}

class CalleeFakes {
public:
    CalleeFakes() { bb::hook::apply(bb::game::image(), fakes_); }
    ~CalleeFakes() { bb::hook::remove(bb::game::image(), fakes_); }
    CalleeFakes(const CalleeFakes&) = delete;
    CalleeFakes& operator=(const CalleeFakes&) = delete;

private:
    template <class F>
    static Replacement fake(std::uint64_t link, F* f, const char* name) {
        return {link, reinterpret_cast<void*>(f), name};
    }
    const std::array<Replacement, 4> fakes_{
        fake(bb::game::ragdoll_update, &fake_update, "fake_update"),
        fake(bb::game::ragdoll_update_spaced, &fake_spaced, "fake_spaced"),
        fake(bb::game::ragdoll_update_idle, &fake_idle, "fake_idle"),
        fake(bb::game::ragdoll_lock_init, &fake_lock_init, "fake_lock_init"),
    };
};

struct Case {
    const char* name;
    bool ragdoll = true;
    bool preferred = false;         // chr+0x3b0 +0x30
    std::uint8_t preferred_on = 1;  // its +0x384
    std::uint8_t lock_flags = 0;    // the lock object's +0xc
    std::int32_t lock_state = 0;    // its +0x10
    std::uint8_t state_flags = 0;   // chr+0x18 +0x20
    bool other = false;             // chr+0x18 +0x30
    std::uint8_t other_flags = 1;   // its +0x18
    std::int32_t state = 4;         // chr+0x18 +0x10
    std::int32_t interval = 0;      // chr+0xe4
    std::uint32_t chr_flags = 0;    // chr+0x1e1
    float below = 0.5f;             // chr+0x3b0 +0x68 +0x2f4
    Mutation mutation = Mutation::none;
};

const Case cases[] = {
    {.name = "every frame"},
    {.name = "every frame, +0x2f4 at 1", .below = 1.0f},
    {.name = "every frame, +0x2f4 NaN", .below = std::numeric_limits<float>::quiet_NaN()},
    {.name = "every frame, preferred lock", .preferred = true},
    {.name = "every frame, preferred lock off", .preferred = true, .preferred_on = 0},
    {.name = "every frame, lock init", .lock_flags = 1},
    {.name = "every frame, lock init done", .lock_flags = 1, .lock_state = 1},
    {.name = "every 3 frames", .interval = 3},
    {.name = "every 30 frames, flag 4", .interval = 30, .chr_flags = 4},
    {.name = "every 2 frames, flags 0x7fb", .interval = 2, .chr_flags = 0x7fb},
    {.name = "every 2 frames, flags 0xfffffffb", .interval = 2, .chr_flags = 0xfffffffb},
    {.name = "negative interval", .interval = -1},
    {.name = "state 3", .state = 3},
    {.name = "state flag 1", .state_flags = 1},
    {.name = "state flag 2, no other", .state_flags = 2},
    {.name = "state flag 2, other off", .state_flags = 2, .other = true, .other_flags = 2},
    {.name = "state flag 2, other on", .state_flags = 2, .other = true},
    {.name = "state flag 2, other on, every 5", .state_flags = 2, .other = true, .interval = 5},
    {.name = "no ragdoll", .ragdoll = false, .lock_flags = 1},
    {.name = "chr gone after the lock init", .lock_flags = 1, .mutation = Mutation::chr_gone},
    {.name = "ragdoll swapped by the lock init",
     .lock_flags = 1,
     .mutation = Mutation::ragdoll_swapped},
    {.name = "ragdoll swapped, idle",
     .lock_flags = 1,
     .interval = -1,
     .mutation = Mutation::ragdoll_swapped},
    {.name = "body swapped by the lock init", .lock_flags = 1, .mutation = Mutation::body_swapped},
};

std::byte* body(Arena& a, float below, float lift) {
    auto* b = static_cast<std::byte*>(a.alloc(0x300));
    at<float>(b, 0x354) = lift;
    const float base[4] = {-209.5f, -39.25f, 5.5f, 1.0f};
    std::memcpy(b + 0x1e0, base, sizeof base);
    at<float>(b, 0x2f4) = below;
    return b;
}

std::tuple<void*, void*> build_module(Arena& a, const Case& c) {
    log_ = a.zeroed<Log>();
    mutation_ = c.mutation;
    auto* chr = static_cast<std::byte*>(a.alloc(0x400));
    auto* physics = static_cast<std::byte*>(a.alloc(0x80));
    at<void*>(chr, 0x3b0) = physics;
    at<void*>(physics, 0x68) = body(a, c.below, 0.75f);
    auto* lock = static_cast<std::byte*>(a.alloc(0x80));
    at<std::uint8_t>(lock, 0x0c) = c.lock_flags;
    at<std::int32_t>(lock, 0x10) = c.lock_state;
    auto* fallback = static_cast<std::byte*>(a.alloc(0x40));
    at<void*>(fallback, 0x20) = lock;
    if (c.preferred) {
        auto* p = static_cast<std::byte*>(a.alloc(0x400));
        at<std::uint8_t>(p, 0x384) = c.preferred_on;
        auto* l = static_cast<std::byte*>(a.alloc(0x80));
        at<std::uint8_t>(l, 0x0c) = 1;  // a second lock, initialised on use
        at<void*>(p, 0x28) = l;
        at<void*>(physics, 0x30) = p;
    }
    auto* state = static_cast<std::byte*>(a.alloc(0x40));
    at<std::uint8_t>(state, 0x20) = c.state_flags;
    at<std::int32_t>(state, 0x10) = c.state;
    if (c.other) {
        auto* o = static_cast<std::byte*>(a.alloc(0x20));
        at<std::uint8_t>(o, 0x18) = c.other_flags;
        at<void*>(state, 0x30) = o;
    }
    at<void*>(chr, 0x18) = state;
    at<std::int32_t>(chr, 0xe4) = c.interval;
    std::memcpy(chr + 0x1e1, &c.chr_flags, sizeof c.chr_flags);  // unaligned
    auto* module = static_cast<std::byte*>(a.alloc(0x100));
    at<void*>(module, 0x08) = chr;
    at<void*>(module, 0x10) = fallback;
    if (c.ragdoll) {
        at<void*>(module, 0x20) = a.alloc(0x200);
    }
    module_ = module;
    other_ = c.mutation == Mutation::body_swapped ? static_cast<void*>(body(a, 2.0f, -1.5f))
                                                  : a.alloc(0x200);
    void* transform = a.alloc(0x30);
    return {module, transform};
}

const Call* only_call(What what) {
    const Call* found = nullptr;
    for (std::uint32_t i = 0; i < log_->count; ++i) {
        if (log_->calls[i].what == what) {
            EXPECT_EQ(found, nullptr) << "two calls of " << what;
            found = &log_->calls[i];
        }
    }
    return found;
}

class RagdollTest : public EbootTest {
protected:
    void SetUp() override {
        EbootTest::SetUp();
        if (IsSkipped()) {
            return;
        }
        bb::game::set_ragdoll_patch_targets();
    }
};

TEST_F(RagdollTest, PoseSyncMatchesAt30) {
    const CalleeFakes fakes;
    for (const Case& c : cases) {
        const Result r = harness().compare<void(void*, void*)>(
            bb::game::chr_physics_pose_sync_link, &bb::game::chr_physics_pose_sync,
            [&](Arena& a) { return build_module(a, c); });
        EXPECT_TRUE(r) << c.name << ": " << r.report();
        // Exactly one update ran, unless there is no ragdoll or no character.
        const bool none = !c.ragdoll || c.mutation == Mutation::chr_gone;
        EXPECT_EQ(log_->count - (only_call(lock_init) != nullptr ? 1u : 0u), none ? 0u : 1u)
            << c.name;
    }
    harness().restore();
}

TEST_F(RagdollTest, PoseSyncPassesTheFrameStepAt60) {
    const CalleeFakes fakes;
    const FrameRateGuard rate(60);
    const auto run = [&](const Case& c, bool replacement) {
        harness().restore();
        const auto [module, transform] = build_module(harness().arena(), c);
        if (replacement) {
            bb::game::chr_physics_pose_sync(module, transform);
        } else {
            bb::game::fn<void(void*, void*)>(bb::game::chr_physics_pose_sync_link)(module,
                                                                                   transform);
        }
    };
    const std::uint32_t step60 = bits(1.0f / 60.0f);
    run(cases[0], true);
    const Call* every = only_call(update);
    ASSERT_NE(every, nullptr);
    EXPECT_EQ(every->args[0], step60);
    EXPECT_EQ(every->args[1], bits(30.0f));
    run(cases[0], false);
    EXPECT_EQ(only_call(update)->args[0], 0x3d088889u);  // the game: 1/30 at any rate

    run(cases[7], true);  // every 3 frames: the spacing stays 3 30 FPS frames
    const Call* three = only_call(spaced);
    ASSERT_NE(three, nullptr);
    EXPECT_EQ(three->args[0], step60);
    EXPECT_EQ(three->args[1], bits(3.0f * (1.0f / 30.0f)));
    run(cases[7], false);
    EXPECT_EQ(only_call(spaced)->args[0], 0x3d088889u);
    EXPECT_EQ(only_call(spaced)->args[1], bits(3.0f * (1.0f / 30.0f)));

    run(cases[11], true);  // idle (negative interval)
    const Call* rest = only_call(idle);
    ASSERT_NE(rest, nullptr);
    EXPECT_EQ(rest->args[0], step60);
    EXPECT_EQ(rest->args[1], bits(900.0f));
    harness().restore();
}

// ---- the patches inside the ragdoll update ------------------------------------------------------

// One run of a patched place, from its first instruction to the one behind it.
struct PatchRun {
    std::uint64_t ragdoll;  // r12
    std::uint64_t frame;    // rbp
    float xmm0[4];          // in and out
    float xmm1[4];
    float xmm2[4];  // out
    float xmm3[4];
    std::uint32_t slot188;  // [rbp-0x188]
    std::uint32_t slot1d0;  // [rbp-0x1d0]
};
static_assert(offsetof(PatchRun, xmm0) == 16 && offsetof(PatchRun, xmm1) == 32 &&
              offsetof(PatchRun, xmm2) == 48 && offsetof(PatchRun, slot188) == 80 &&
              sizeof(PatchRun) == 88);

}  // namespace

extern "C" {
std::uint64_t ragdoll_test_entry = 0;
std::uint64_t ragdoll_test_rsp = 0;
void* ragdoll_test_run = nullptr;
void ragdoll_test_enter(void* run);
void ragdoll_test_capture();
}
asm(R"(
    .pushsection .text
    .intel_syntax noprefix
    .globl ragdoll_test_enter
ragdoll_test_enter:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    mov qword ptr [rip + ragdoll_test_rsp], rsp
    mov qword ptr [rip + ragdoll_test_run], rdi
    mov r12, qword ptr [rdi]
    mov rbp, qword ptr [rdi + 8]
    vmovups xmm0, xmmword ptr [rdi + 16]
    vmovups xmm1, xmmword ptr [rdi + 32]
    vpcmpeqd xmm2, xmm2, xmm2
    vpcmpeqd xmm3, xmm3, xmm3
    jmp qword ptr [rip + ragdoll_test_entry]
    .globl ragdoll_test_capture
ragdoll_test_capture:
    mov r8, qword ptr [rip + ragdoll_test_run]
    vmovups xmmword ptr [r8 + 16], xmm0
    vmovups xmmword ptr [r8 + 32], xmm1
    vmovups xmmword ptr [r8 + 48], xmm2
    vmovups xmmword ptr [r8 + 64], xmm3
    mov r9d, dword ptr [rbp - 0x188]
    mov dword ptr [r8 + 80], r9d
    mov r9d, dword ptr [rbp - 0x1d0]
    mov dword ptr [r8 + 84], r9d
    mov rsp, qword ptr [rip + ragdoll_test_rsp]
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

// vmovaps xmm4, [r12+0xe0]; vmovaps [rbp-0x1a0], xmm4
constexpr std::array<std::uint8_t, 18> behind_impulse = {0xc4, 0xc1, 0x78, 0x28, 0xa4, 0x24,
                                                         0xe0, 0x00, 0x00, 0x00, 0xc5, 0xf8,
                                                         0x29, 0xa5, 0x60, 0xfe, 0xff, 0xff};
// vmovaps [rbp-0x90], xmm0; mov rdi, rdx; mov esi, r15d
constexpr std::array<std::uint8_t, 14> behind_pull = {0xc5, 0xf8, 0x29, 0x85, 0x70, 0xff, 0xff,
                                                      0xff, 0x48, 0x89, 0xd7, 0x44, 0x89, 0xfe};

class RagdollPatchTest : public RagdollTest {
protected:
    void SetUp() override {
        RagdollTest::SetUp();
        if (IsSkipped()) {
            return;
        }
        captures_.emplace(
            std::array{Replacement{bb::game::ragdoll_impulse_continue,
                                   reinterpret_cast<void*>(&ragdoll_test_capture),
                                   "capture_impulse", behind_impulse, bb::game::ragdoll_update},
                       Replacement{bb::game::ragdoll_root_pull_continue,
                                   reinterpret_cast<void*>(&ragdoll_test_capture), "capture_pull",
                                   behind_pull, bb::game::ragdoll_update}});
    }
    void TearDown() override { captures_.reset(); }

    // Runs the place at `entry` with the ragdoll's strengths `near`, `far` and xmm0, xmm1.
    PatchRun run(std::uint64_t entry, float near, float far, const float (&x0)[4],
                 const float (&x1)[4]) {
        ragdoll_.fill(std::byte{0});
        at<float>(ragdoll_.data(), 0xfc) = near;
        at<float>(ragdoll_.data(), 0x100) = far;
        frame_.fill(std::byte{0x5a});
        PatchRun out{};
        out.ragdoll = u(ragdoll_.data());
        out.frame = u(frame_.data() + 0x200);
        std::memcpy(out.xmm0, x0, sizeof x0);
        std::memcpy(out.xmm1, x1, sizeof x1);
        ragdoll_test_entry = bb::game::image().runtime(entry);
        ragdoll_test_enter(&out);
        return out;
    }

    alignas(16) std::array<std::byte, 0x200> ragdoll_{};
    alignas(16) std::array<std::byte, 0x300> frame_{};
    std::optional<Hooked<2>> captures_;
};

constexpr float strengths[][2] = {{0.0f, 0.0f},
                                  {1.5f, 40.0f},
                                  {-3.25f, 1e30f},
                                  {7.0e-39f, -0.0f},
                                  {std::numeric_limits<float>::infinity(), 2.0f}};
constexpr float deltas[][4] = {{0.25f, -0.5f, 1.0f, 0.0f}, {-12.0f, 3.5f, 1e-3f, 2.0f}};
constexpr float masses[][4] = {
    {0.0125f, 9.0f, 9.0f, 9.0f}, {1.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 2.0f, 3.0f}};

void expect_same(const PatchRun& a, const PatchRun& b, const char* what) {
    EXPECT_EQ(std::memcmp(&a, &b, sizeof a), 0) << what;
}

TEST_F(RagdollPatchTest, TheImpulseIsTheGamesAt30) {
    const Hooked<1> stub(std::array{registered(bb::game::ragdoll_impulse_load)});
    std::array once{registered(bb::game::ragdoll_impulse_load)};
    for (const auto& s : strengths) {
        const PatchRun stubbed =
            run(bb::game::ragdoll_impulse_load, s[0], s[1], deltas[0], masses[0]);
        bb::hook::remove(bb::game::image(), once);
        const PatchRun game = run(bb::game::ragdoll_impulse_load, s[0], s[1], deltas[0], masses[0]);
        bb::hook::apply(bb::game::image(), once);
        expect_same(stubbed, game, "impulse");
        EXPECT_EQ(game.slot188, bits(s[0]));
        EXPECT_EQ(game.slot1d0, bits(s[1]));
    }
}

TEST_F(RagdollPatchTest, TheImpulseIsHalvedAt60) {
    const FrameRateGuard rate(60);
    const Hooked<1> stub(std::array{registered(bb::game::ragdoll_impulse_load)});
    for (const auto& s : strengths) {
        const PatchRun r = run(bb::game::ragdoll_impulse_load, s[0], s[1], deltas[0], masses[0]);
        EXPECT_EQ(r.slot188, bits(s[0] * 0.5f));
        EXPECT_EQ(r.slot1d0, bits(s[1] * 0.5f));
        EXPECT_EQ(bits(r.xmm2[0]), bits(s[0] * 0.5f));
        EXPECT_EQ(bits(r.xmm3[0]), bits(s[1] * 0.5f));
        EXPECT_EQ(r.xmm2[1], 0.0f);  // vmovss loads clear the upper lanes, as the game's do
        EXPECT_EQ(r.xmm3[3], 0.0f);
    }
}

TEST_F(RagdollPatchTest, TheRootPullIsTheGamesAt30) {
    const Hooked<1> stub(std::array{registered(bb::game::ragdoll_root_pull)});
    std::array once{registered(bb::game::ragdoll_root_pull)};
    for (const auto& d : deltas) {
        for (const auto& m : masses) {
            const PatchRun stubbed = run(bb::game::ragdoll_root_pull, 0, 0, d, m);
            bb::hook::remove(bb::game::image(), once);
            const PatchRun game = run(bb::game::ragdoll_root_pull, 0, 0, d, m);
            bb::hook::apply(bb::game::image(), once);
            expect_same(stubbed, game, "root pull");
            for (int i = 0; i < 4; ++i) {
                EXPECT_EQ(bits(game.xmm0[i]), bits(d[i] * (m[0] * 30.0f)));
            }
        }
    }
}

TEST_F(RagdollPatchTest, TheRootPullIsHalvedAt60) {
    const FrameRateGuard rate(60);
    const Hooked<1> stub(std::array{registered(bb::game::ragdoll_root_pull)});
    for (const auto& d : deltas) {
        for (const auto& m : masses) {
            const PatchRun r = run(bb::game::ragdoll_root_pull, 0, 0, d, m);
            for (int i = 0; i < 4; ++i) {
                EXPECT_EQ(bits(r.xmm0[i]), bits(d[i] * (m[0] * 15.0f)));
                EXPECT_EQ(bits(r.xmm1[i]), bits(m[0] * 15.0f));  // broadcast
            }
        }
    }
}

TEST(RagdollStepRatio, IsExactlyAHalfAt60AndOneAt30) {
    bb::game::set_frame_rate(60);
    EXPECT_EQ(bb_step_ratio, 0.5f);
    bb::game::set_frame_rate(30);
    EXPECT_EQ(bb_step_ratio, 1.0f);
    // 30 * (30 * ratio) gives exactly 30 and 15, where 900 * frame_step() would not at 30.
    EXPECT_NE(900.0f * bb_frame_step, 30.0f);
}

}  // namespace
