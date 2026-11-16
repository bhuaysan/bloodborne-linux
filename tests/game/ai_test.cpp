// Differential tests of the AI world step wrapper in src/game/ai/, and of what it does at 60 FPS.
// The Havok AI world's calls in the step (0x008a07a0 and its siblings) are hooked with fakes that
// record their arguments, so the tests see the time step the Havok AI world gets without one.
// chr_ai_step (0x01e2bc10) runs as the game's code, up to its call of the step.
#include <array>
#include <cstdint>
#include <cstring>
#include <tuple>

#include <gtest/gtest.h>

#include "game/ai/world_step.hpp"
#include "game/frame/frame.hpp"
#include "game/image.hpp"
#include "harness/eboot.hpp"
#include "harness/harness.hpp"
#include "hook/hook.hpp"

namespace {

using bb::game::FD4TaskData;
using bb::harness::Arena;
using bb::harness::Result;
using bb::test::EbootTest;

constexpr std::uint64_t ai_world_step = 0x1dcdad0;
constexpr std::uint64_t chr_ai_step = 0x1e2bc10;
constexpr std::uint64_t hk_ai_manager_singleton = 0x55401a0;  // SprjHkAiManager
constexpr std::uint64_t task_data_vtable = 0x52efd40;

// ---- the call log -------------------------------------------------------------------------------

// 0x8c8350 and 0x8a0790 are thunks that pass null for the last two arguments of 0x8c8360 and
// 0x8a07a0, too short to hook: their calls show up as those of their targets.
enum What : std::uint64_t {
    mark_sections = 1,  // 0x8c8360
    prepare,            // 0x897390
    prepare_in,         // 0x895bd0
    world_step,         // 0x8a07a0
};

struct Call {
    std::uint64_t what;
    std::uint64_t args[8];
};

// In the arena, so the harness compares it between the original and the replacement.
struct Log {
    std::uint32_t count;
    Call calls[16];
};
Log* log_ = nullptr;

void record(What what, std::uint64_t a = 0, std::uint64_t b = 0, std::uint64_t c = 0,
            std::uint64_t d = 0, std::uint64_t e = 0, std::uint64_t f = 0, std::uint64_t g = 0,
            std::uint64_t h = 0) {
    ASSERT_LT(log_->count, std::size(log_->calls));
    log_->calls[log_->count++] = {what, {a, b, c, d, e, f, g, h}};
}

std::uint64_t u(const void* p) {
    return reinterpret_cast<std::uint64_t>(p);
}

std::uint32_t float_bits(float f) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof bits);
    return bits;
}

// The step's arrays live on its stack, whose address differs between runs through the wrapper
// and without it: the fakes log what the arrays hold (data, then size and capacity).
std::uint64_t word(const void* array, std::size_t index) {
    std::uint64_t w = 0;
    std::memcpy(&w, static_cast<const std::byte*>(array) + index * 8, sizeof w);
    return w;
}

void fake_mark_sections(void* world, void* bits, void* a, void* b) {
    record(mark_sections, u(world), word(bits, 0), word(bits, 1), u(a), u(b));
}
void fake_prepare(void* world) {
    record(prepare, u(world));
}
void fake_prepare_in(void* world, void* a, void* b) {
    record(prepare_in, u(world), u(a), u(b));
}
void fake_world_step(float dt, void* world, void* added, void* removed, void* a, void* b) {
    record(world_step, float_bits(dt), u(world), word(added, 0), word(added, 1),
           word(removed, 0), word(removed, 1), u(a), u(b));
}

// Hooks the step's Havok AI calls with the fakes for one test.
class WorldFakes {
public:
    WorldFakes() { bb::hook::apply(bb::game::image(), fakes_); }
    ~WorldFakes() { bb::hook::remove(bb::game::image(), fakes_); }
    WorldFakes(const WorldFakes&) = delete;
    WorldFakes& operator=(const WorldFakes&) = delete;

private:
    template <class F>
    static bb::hook::Replacement fake(std::uint64_t link, F* f, const char* name) {
        return {link, reinterpret_cast<void*>(f), name};
    }
    const std::array<bb::hook::Replacement, 4> fakes_{
        fake(0x8c8360, &fake_mark_sections, "fake_mark_sections"),
        fake(0x897390, &fake_prepare, "fake_prepare"),
        fake(0x895bd0, &fake_prepare_in, "fake_prepare_in"),
        fake(0x8a07a0, &fake_world_step, "fake_world_step"),
    };
};

// The wrapper as bb-run registers it (BB_WRAP), hooked for the lifetime of the object.
class WrapperHook {
public:
    WrapperHook() {
        for (const bb::hook::Replacement& r : bb::hook::replacements()) {
            if (r.link_addr == ai_world_step) {
                hook_[0] = r;
            }
        }
        EXPECT_EQ(hook_[0].impl, reinterpret_cast<void*>(&bb::game::ai_world_step));
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

// ---- the AI world manager -----------------------------------------------------------------------

template <class T>
T& at(void* object, std::size_t offset) {
    return *reinterpret_cast<T*>(static_cast<std::byte*>(object) + offset);
}

struct Case {
    const char* name;
    std::uint8_t in_area = 0;  // +0xf0: the step's variant with the manager's +0x78, +0x80
    std::uint8_t rescan = 0;   // +0xf2: marks the sections in its bounds first, then clears it
};

const Case cases[] = {
    {.name = "step"},
    {.name = "step in area", .in_area = 1},
    {.name = "step after a rescan request", .rescan = 1},
};

// A manager with an empty section map and character list, no sections and no characters to add:
// the step makes no allocation and only calls the Havok AI world.
void* build_manager(Arena& a, const Case& c) {
    log_ = a.zeroed<Log>();
    void* const m = a.alloc(0x100);
    void* const map = a.alloc(0x40);  // std::map header, empty: its root link points to itself
    at<void*>(map, 0) = map;
    at<void*>(m, 0x10) = map;
    void* const list = a.alloc(0x20);  // list sentinel, empty
    at<void*>(list, 0) = list;
    at<void*>(m, 0x30) = list;
    at<std::int64_t>(m, 0x38) = 0;
    void* const world = a.alloc(0x60);
    at<void*>(world, 0x20) = a.alloc(0x40);  // +0x20: section count 0
    at<void*>(world, 0x48) = a.alloc(0x60);  // +0x50: character count 0
    at<void*>(m, 0x50) = world;
    at<void*>(m, 0x78) = a.alloc(0x10);
    at<void*>(m, 0x80) = a.alloc(0x10);
    at<std::uint32_t>(m, 0xa8) = 5;
    at<std::uint8_t>(m, 0xf0) = c.in_area;
    at<std::uint8_t>(m, 0xf2) = c.rescan;
    return m;
}

FD4TaskData* task_data(Arena& a, float delta_time) {
    auto* task = a.zeroed<FD4TaskData>();
    task->vtable = reinterpret_cast<const void*>(bb::game::image().runtime(task_data_vtable));
    task->delta_time = delta_time;
    return task;
}

// chr_ai_step's world (SprjWorldNvmManager) and the SprjHkAiManager singleton that holds the
// AI world manager at +0x10.
std::tuple<void*, void*, std::uint8_t, std::uint8_t> build_chr_ai_step(Arena& a, const Case& c) {
    void* const m = build_manager(a, c);
    void* const hk = a.alloc(0x40);
    at<void*>(hk, 0x10) = m;
    bb::game::global<void*>(hk_ai_manager_singleton) = hk;
    void* const nvm = a.alloc(0x2f00);
    // Neither the loop over the world's +0x2bc8 entries before the step (third argument) nor the
    // part after it (fourth): the function ends after the step.
    return {nvm, a.alloc(0x80), std::uint8_t{0}, std::uint8_t{0}};
}

// The time step the Havok AI world got (float bits), or 0 when it wasn't called once.
std::uint32_t world_step_delta() {
    std::uint32_t steps = 0;
    std::uint32_t delta = 0;
    for (std::uint32_t i = 0; i < log_->count; ++i) {
        if (log_->calls[i].what == world_step) {
            ++steps;
            delta = static_cast<std::uint32_t>(log_->calls[i].args[0]);
        }
    }
    return steps == 1 ? delta : 0;
}

using ChrAiStep = void(void* nvm, void* context, std::uint8_t before, std::uint8_t after);

void chr_ai_step_unhooked(void* nvm, void* context, std::uint8_t before, std::uint8_t after) {
    bb::game::fn<ChrAiStep>(chr_ai_step)(nvm, context, before, after);
}

void chr_ai_step_hooked(void* nvm, void* context, std::uint8_t before, std::uint8_t after) {
    const WrapperHook hook;
    bb::game::fn<ChrAiStep>(chr_ai_step)(nvm, context, before, after);
}

// A call from anywhere but chr_ai_step, as the AI test module's (0x019a72f9), keeps its task data
// at every frame rate.
TEST_F(EbootTest, AiWorldStepMatches) {
    const WorldFakes fakes;
    for (const unsigned fps : {30u, 60u}) {
        const FrameRateGuard rate(fps);
        for (const Case& c : cases) {
            const Result r = harness().compare<bb::game::AiWorldStep>(
                ai_world_step, &bb::game::ai_world_step, [&](Arena& a) {
                    void* const m = build_manager(a, c);
                    return std::tuple{m, static_cast<const FD4TaskData*>(task_data(a, 0.025f))};
                });
            EXPECT_TRUE(r) << c.name << " at " << fps << ": " << r.report();
            EXPECT_EQ(world_step_delta(), float_bits(0.025f)) << c.name << " at " << fps;
        }
    }
    harness().restore();
}

// chr_ai_step through the wrapper: at 30 it does what it does without it, byte for byte.
TEST_F(EbootTest, ChrAiStepUnchangedAt30) {
    const WorldFakes fakes;
    for (const Case& c : cases) {
        const Result r = harness().compare_functions<ChrAiStep>(
            &chr_ai_step_unhooked, &chr_ai_step_hooked,
            [&](Arena& a) { return build_chr_ai_step(a, c); });
        EXPECT_TRUE(r) << c.name << ": " << r.report();
        EXPECT_EQ(world_step_delta(), 0x3d088889u) << c.name;  // the game's 1/30
    }
    harness().restore();
}

// At 60 the Havok AI world gets 1/60 from chr_ai_step through the wrapper, and the game's 1/30
// without it.
TEST_F(EbootTest, ChrAiStepAt60) {
    const WorldFakes fakes;
    Log seen{};  // the calls of the last run, with the time step left out
    const auto delta = [&](const Case& c, bool hooked) {
        harness().restore();
        const auto [nvm, context, before, after] = build_chr_ai_step(harness().arena(), c);
        (hooked ? chr_ai_step_hooked : chr_ai_step_unhooked)(nvm, context, before, after);
        const std::uint32_t d = world_step_delta();
        seen = *log_;
        for (std::uint32_t i = 0; i < seen.count; ++i) {
            if (seen.calls[i].what == world_step) {
                seen.calls[i].args[0] = 0;
            }
        }
        return d;
    };
    for (const Case& c : cases) {
        EXPECT_EQ(delta(c, true), 0x3d088889u) << c.name;
        {
            const FrameRateGuard rate(60);
            EXPECT_EQ(delta(c, false), 0x3d088889u) << c.name;
            const Log unhooked = seen;
            EXPECT_EQ(delta(c, true), float_bits(1.0f / 60.0f)) << c.name;
            // Apart from the step, the Havok AI world gets the same calls.
            EXPECT_EQ(std::memcmp(&unhooked, &seen, sizeof seen), 0) << c.name;
        }
    }
    harness().restore();
}

// The wrapper's choice by return address, with a stand-in for the original.
const FD4TaskData* seen_task_ = nullptr;
FD4TaskData seen_copy_{};
void* seen_manager_ = nullptr;
void stand_in(void* manager, const FD4TaskData* task) {
    seen_manager_ = manager;
    seen_task_ = task;
    seen_copy_ = *task;
}

TEST_F(EbootTest, AiWorldStepChoosesByCallSite) {
    const std::uintptr_t from_chr_ai_step =
        bb::game::image().runtime(bb::game::chr_ai_step_call_return);
    const std::uintptr_t elsewhere = bb::game::image().runtime(0x019a72fe);
    int manager = 0;
    const FD4TaskData task{reinterpret_cast<const void*>(0x1234), 1.0f / 30.0f};
    const auto run = [&](std::uintptr_t from) {
        seen_task_ = nullptr;
        bb::game::ai_world_step_from(&stand_in, from, &manager, &task);
        EXPECT_EQ(seen_manager_, &manager);
    };
    run(from_chr_ai_step);
    EXPECT_EQ(seen_task_, &task);
    {
        const FrameRateGuard rate(60);
        run(elsewhere);
        EXPECT_EQ(seen_task_, &task);
        run(from_chr_ai_step);
        EXPECT_NE(seen_task_, &task);
        EXPECT_EQ(seen_copy_.vtable, task.vtable);
        EXPECT_EQ(float_bits(seen_copy_.delta_time), float_bits(1.0f / 60.0f));
    }
}

}  // namespace
