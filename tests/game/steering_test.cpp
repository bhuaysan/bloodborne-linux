// Differential tests of the agent steering wrapper in src/game/ai/: through the registered hook the
// game's steering (0x01dbbd00) leaves at 30 FPS the same bytes and points as without it, and at 60
// FPS two frames leave what one frame leaves at 30. Without the wrapper the steering at 60 loses its
// direction every 30 calls (#570); a test shows that, and that the wrapper keeps it.
#include "game/ai/steering.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <tuple>

#include <gtest/gtest.h>

#include "game/frame/frame.hpp"
#include "game/image.hpp"
#include "harness/eboot.hpp"
#include "harness/harness.hpp"
#include "hook/hook.hpp"

namespace {

using bb::game::FD4TaskData;
using bb::game::SteerPoint;
using bb::harness::Arena;
using bb::harness::Result;
using bb::test::EbootTest;

constexpr std::uint64_t agent_steer = 0x1dbbd00;
constexpr std::uint64_t agent_vtable = 0x534e220;
constexpr std::uint64_t task_data_vtable = 0x52efd40;

// The wrapper as bb-run registers it (BB_WRAP), hooked for the lifetime of the object.
class WrapperHook {
public:
    WrapperHook() {
        for (const bb::hook::Replacement& r : bb::hook::replacements()) {
            if (r.link_addr == agent_steer) {
                hook_[0] = r;
            }
        }
        EXPECT_EQ(hook_[0].impl, reinterpret_cast<void*>(&bb::game::agent_steer));
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

template <class T>
T& at(void* object, std::size_t offset) {
    return *reinterpret_cast<T*>(static_cast<std::byte*>(object) + offset);
}

// ---- an agent walking along a path ----------------------------------------------------------------

constexpr unsigned max_frames = 128;

struct Case {
    const char* name;
    std::int32_t lag;  // +0x250, in 30 FPS frames (the game sets 1 to 29 from the speed)
    float turn;        // the path point's x per metre walked: 0 walks straight, else it bends
};

const Case cases[] = {
    {.name = "short lag, straight", .lag = 5, .turn = 0.0f},
    {.name = "lag 20, straight", .lag = 20, .turn = 0.0f},
    {.name = "lag 29, bending", .lag = 29, .turn = 0.3f},
};

// What the drivers leave in the arena: the points the steering returned. `first` holds the point
// of each 30 FPS frame (at 60 the first of its two calls), `second` the point at 60 of the second
// call (at 30 the frame's point again).
struct Scene {
    void* agent;
    void* character;
    void* path_point;
    float turn;
    std::array<std::array<float, 4>, max_frames> first;
    std::array<std::array<float, 4>, max_frames> second;
};

// An agent of the game's class (vtable 0x0534e220) whose Havok AI character is in state 3 (+0xac),
// so the steering takes the direction to the next point of its path (0x01dbca40 through
// 0x0089c030: the path's point at index +0x70 + 1, 0x30 bytes each) and runs it through the ring.
std::tuple<Scene*, unsigned> build(Arena& a, const Case& c, unsigned frames) {
    auto* const s = a.zeroed<Scene>();
    s->agent = a.alloc(0x300);
    at<std::uintptr_t>(s->agent, 0) = bb::game::image().runtime(agent_vtable);
    s->character = a.alloc(0xc0);
    at<std::array<float, 4>>(s->character, 0x20) = {10.0f, 2.0f, 50.0f, 1.0f};
    at<float>(s->character, 0x64) = 0.45f;
    at<std::int32_t>(s->character, 0xac) = 3;
    at<void*>(s->agent, 0x230) = s->character;
    void* const nav = a.alloc(0x80);
    at<void*>(nav, 0x58) = a.alloc(0x20);  // gets the character's +0x64 at +0x1c
    void* const path = a.alloc(0x30);
    void* const points = a.alloc(0x60);
    at<void*>(path, 0x10) = points;  // +0x20 = 0: points in world space
    at<void*>(nav, 0x60) = path;
    at<std::int32_t>(nav, 0x70) = 0;
    s->path_point = static_cast<std::byte*>(points) + 0x30;
    at<std::array<float, 4>>(s->path_point, 0) = {10.0f, 2.0f, -50.0f, 1.0f};
    at<void*>(s->agent, 0x238) = nav;
    at<std::int32_t>(s->agent, 0x250) = c.lag;
    s->turn = c.turn;
    return {s, frames};
}

// Walks the agent to where it is after `t` seconds at 1 m/s, and moves the path point with it.
void walk(Scene* s, float t) {
    at<float>(s->character, 0x28) = 50.0f - t;
    at<float>(s->path_point, 0) = 10.0f + s->turn * t;
}

std::array<float, 4> as_floats(SteerPoint p) {
    std::array<float, 4> f;
    std::memcpy(f.data(), &p, sizeof p);
    return f;
}

// The task data the path follower hands on, with `step` (on the caller's stack, as in the game).
FD4TaskData task_data(float step) {
    return {reinterpret_cast<const void*>(bb::game::image().runtime(task_data_vtable)), step};
}

SteerPoint steer(Scene* s, const FD4TaskData& task) {
    return bb::game::fn<bb::game::AgentSteer>(agent_steer)(s->agent, &task);
}

// Lines up the frame count on a multiple of the frame rate's calls, as the drivers start there.
void align_frames() {
    while (bb::game::frame_count() % (bb::game::frame_rate() / 30) != 0) {
        bb::game::count_frame();
    }
}

using Frames = void(Scene* s, unsigned frames);

// `frames` frames at 30 without the hook.
void frames_unhooked(Scene* s, unsigned frames) {
    const FD4TaskData task = task_data(1.0f / 30.0f);
    for (unsigned k = 0; k < frames; ++k) {
        walk(s, static_cast<float>(k) / 30.0f);
        s->first[k] = s->second[k] = as_floats(steer(s, task));
    }
}

// The same game time through the hook at the frame rate: frame_rate() / 30 calls a 30 FPS frame,
// with the frame's step, and the agent moving on with every 30 FPS frame.
void frames_hooked(Scene* s, unsigned frames) {
    const WrapperHook hook;
    const unsigned calls = bb::game::frame_rate() / 30;
    const FD4TaskData task = task_data(bb::game::frame_step());
    align_frames();
    for (unsigned k = 0; k < frames; ++k) {
        walk(s, static_cast<float>(k) / 30.0f);
        for (unsigned i = 0; i < calls; ++i) {
            (i == 0 ? s->first : s->second)[k] = as_floats(steer(s, task));
            bb::game::count_frame();
        }
        if (calls == 1) {
            s->second[k] = s->first[k];
        }
    }
}

constexpr unsigned frames_at_30 = 120;  // 4 s: the ring goes round four times

// At 30 the hooked steering is the game's, byte for byte, and returns the same points.
TEST_F(EbootTest, AgentSteerUnchangedAt30) {
    for (const Case& c : cases) {
        const Result r = harness().compare_functions<Frames>(
            &frames_unhooked, &frames_hooked, [&](Arena& a) { return build(a, c, frames_at_30); });
        EXPECT_TRUE(r) << c.name << ": " << r.report();
    }
    harness().restore();
}

// At 60 two frames through the hook leave what one frame leaves at 30: the same ring, state, step
// and points, and on the second frame the point of the first.
TEST_F(EbootTest, AgentSteerAt60MatchesTheSteeringAt30) {
    const FrameRateGuard rate(60);
    for (const Case& c : cases) {
        const Result r = harness().compare_functions<Frames>(
            &frames_unhooked, &frames_hooked, [&](Arena& a) { return build(a, c, frames_at_30); });
        EXPECT_TRUE(r) << c.name << ": " << r.report();
    }
    harness().restore();
}

// How often the steering returned the agent's own position (w = 1, no direction): `calls` calls
// at `fps` with that frame step, the agent walking at 1 m/s.
int lost_directions(Scene* s, unsigned fps, unsigned calls, bool hooked) {
    const FD4TaskData task = task_data(1.0f / static_cast<float>(fps));
    int lost = 0;
    for (unsigned i = 0; i < calls; ++i) {
        walk(s, static_cast<float>(i) / static_cast<float>(fps));
        const std::array<float, 4> p = as_floats(steer(s, task));
        const auto* const position = &at<float>(s->character, 0x20);
        if (p[0] == position[0] && p[1] == position[1] && p[2] == position[2] && p[3] == 1.0f) {
            ++lost;
        }
        if (hooked) {
            bb::game::count_frame();
        }
    }
    return lost;
}

// The bug the wrapper fixes: with a lag of 15 or more 30 FPS frames the game's steering at 60
// steers the agent at itself once every 30 calls. At 30, and at 60 through the hook, it never does.
TEST_F(EbootTest, AgentSteerKeepsItsDirectionAt60) {
    const Case& c = cases[1];  // lag 20
    {
        harness().restore();
        const int lost = lost_directions(std::get<0>(build(harness().arena(), c, 0)), 30, 120, false);
        EXPECT_EQ(lost, 0);
    }
    {
        harness().restore();
        const int lost = lost_directions(std::get<0>(build(harness().arena(), c, 0)), 60, 240, false);
        EXPECT_GE(lost, 6) << "the game's steering at 60 without the wrapper";
    }
    {
        const FrameRateGuard rate(60);
        harness().restore();
        Scene* const s = std::get<0>(build(harness().arena(), c, 0));
        const WrapperHook hook;
        align_frames();
        EXPECT_EQ(lost_directions(s, 60, 240, true), 0);
    }
    harness().restore();
}

TEST_F(EbootTest, AgentSteerPrologueIsTheEboots) {
    const auto& prologue = bb::game::agent_steer_prologue;
    EXPECT_EQ(std::memcmp(reinterpret_cast<const void*>(bb::game::image().runtime(agent_steer)),
                          prologue.data(), prologue.size()),
              0);
}

// ---- the call pattern ---------------------------------------------------------------------------

int stand_in_calls = 0;
float stand_in_step = 0.0f;
SteerPoint stand_in(void*, const FD4TaskData* task) {
    ++stand_in_calls;
    stand_in_step = task->delta_time;
    return {1, 2};
}

TEST(AgentSteer, RunsTheGamesSteeringOnEveryCallsthFrame) {
    // An agent without a Havok AI character: every skipped frame calls the game's steering too.
    alignas(16) std::array<std::byte, 0x300> agent{};
    const FD4TaskData task{nullptr, 1.0f / 60.0f};
    for (const unsigned calls : {1u, 2u}) {
        std::string pattern;
        for (std::uint64_t frame = 5; frame < 9; ++frame) {
            stand_in_calls = 0;
            bb::game::agent_steer_every(&stand_in, calls, frame, agent.data(), &task);
            pattern += stand_in_step == task.delta_time * static_cast<float>(calls) ? 'x' : '.';
        }
        EXPECT_EQ(pattern, calls == 1 ? "xxxx" : ".x.x") << calls;
    }
}

TEST(AgentSteer, SkippedFramesSteerFromTheStoredDirection) {
    alignas(16) std::array<std::byte, 0x300> agent{};
    alignas(16) std::array<std::byte, 0xc0> character{};
    alignas(16) std::array<std::byte, 0x80> nav{};
    alignas(16) std::array<std::byte, 0x20> holder{};
    at<void*>(agent.data(), 0x230) = character.data();
    at<void*>(agent.data(), 0x238) = nav.data();
    at<void*>(nav.data(), 0x58) = holder.data();
    at<std::array<float, 4>>(character.data(), 0x20) = {1.0f, 2.0f, 3.0f, 1.0f};
    at<float>(character.data(), 0x64) = 0.5f;
    at<std::array<float, 4>>(agent.data(), 0x20) = {0.6f, 0.0f, -0.8f, 0.0f};
    const FD4TaskData task{nullptr, 1.0f / 60.0f};
    stand_in_calls = 0;
    const SteerPoint p = bb::game::agent_steer_every(&stand_in, 2, 3, agent.data(), &task);
    EXPECT_EQ(stand_in_calls, 0);
    const std::array<float, 4> f = as_floats(p);
    EXPECT_EQ(f[0], 0.6f * 3.0f + 1.0f);
    EXPECT_EQ(f[1], 2.0f);
    EXPECT_EQ(f[2], -0.8f * 3.0f + 3.0f);
    EXPECT_EQ(f[3], 1.0f);
    EXPECT_EQ(at<std::uint8_t>(agent.data(), 0x28c), 1);
    EXPECT_EQ(at<float>(holder.data(), 0x1c), 0.5f);
}

}  // namespace
