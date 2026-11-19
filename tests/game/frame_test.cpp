// Differential tests of the frame pacer and the main loop tick in src/game/frame/, and of what
// they do at 60 FPS. The wall clock is a fake that moves on by a fixed step with every read, so
// the original and the replacement wait the same number of reads.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <format>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "game/frame/frame.hpp"
#include "game/image.hpp"
#include "harness/eboot.hpp"
#include "harness/harness.hpp"
#include "hle/kernel/time.hpp"

namespace {

using bb::game::FD4TaskData;
using bb::game::SprjFlipper;
using bb::harness::Arena;
using bb::harness::Result;
using bb::hle::kernel::GuestTimeval;
using bb::test::EbootTest;

constexpr std::uint64_t flipper_wait_frame = 0x2034770;
constexpr std::uint64_t main_loop_tick = 0x2018d20;

// ---- the fake clock -----------------------------------------------------------------------------

struct FakeClock {
    std::uint64_t now_us = 0;
    std::uint64_t step_us = 0;
    std::uint32_t* reads = nullptr;  // in the arena, so the harness compares the count
};
FakeClock fake_clock;

GuestTimeval read_fake_clock() {
    const std::uint64_t t = fake_clock.now_us;
    fake_clock.now_us += fake_clock.step_us;
    ++*fake_clock.reads;
    return {static_cast<std::int64_t>(t / 1000000), static_cast<std::int64_t>(t % 1000000)};
}

// Installs the fake clock for one test.
class FakeClockGuard {
public:
    FakeClockGuard() { bb::hle::kernel::set_wall_clock(&read_fake_clock); }
    ~FakeClockGuard() { bb::hle::kernel::set_wall_clock(nullptr); }
    FakeClockGuard(const FakeClockGuard&) = delete;
    FakeClockGuard& operator=(const FakeClockGuard&) = delete;
};

// Sets the frame rate for one test and puts back the default, 30, also when the test fails early.
class FrameRateGuard {
public:
    explicit FrameRateGuard(unsigned fps) { bb::game::set_frame_rate(fps); }
    ~FrameRateGuard() { bb::game::set_frame_rate(bb::game::default_frame_rate); }
    FrameRateGuard(const FrameRateGuard&) = delete;
    FrameRateGuard& operator=(const FrameRateGuard&) = delete;
};

// Starts the clock at `start_us`, moving `step_us` per read, with its read counter in `a`.
void start_clock(Arena& a, std::uint64_t start_us, std::uint64_t step_us) {
    fake_clock = {start_us, step_us, a.zeroed<std::uint32_t>()};
}

// ---- fake game objects --------------------------------------------------------------------------

// A game object whose virtual functions are ours. Each call counts and returns `result`; the task
// line's run (vtable +0x30) also records its arguments.
struct FakeObject {
    void* const* vtable;
    std::uint8_t result;
    std::uint32_t calls;
    std::uint64_t arg1, arg2, arg3;
    const void* data_vtable;
    std::uint32_t delta_bits;
};

std::uint8_t fake_virtual(void* self) {
    auto* o = static_cast<FakeObject*>(self);
    ++o->calls;
    return o->result;
}

void fake_run_tasks(void* self, std::uint64_t a, std::uint64_t b, std::uint64_t c,
                    const FD4TaskData* data) {
    auto* o = static_cast<FakeObject*>(self);
    ++o->calls;
    o->arg1 = a;
    o->arg2 = b;
    o->arg3 = c & 0xffffffff;  // the game passes an int
    o->data_vtable = data->vtable;
    std::memcpy(&o->delta_bits, &data->delta_time, sizeof o->delta_bits);
}

FakeObject* fake_object(Arena& a, std::uint8_t result) {
    auto* vtable = a.zeroed<void*>(32);
    for (int i = 0; i < 32; ++i) {
        vtable[i] = reinterpret_cast<void*>(&fake_virtual);
    }
    vtable[0x30 / 8] = reinterpret_cast<void*>(&fake_run_tasks);
    auto* o = a.zeroed<FakeObject>();
    o->vtable = vtable;
    o->result = result;
    return o;
}

template <class T>
T& global(std::uint64_t link) {
    return bb::game::global<T>(link);
}

// ---- the pacer's state --------------------------------------------------------------------------

constexpr std::uint64_t clock_start = 1'700'000'000'000'000;

// A pacer as flipper_construct leaves it and the main loop's first tick (no_skip) sets it, with
// its last frame read 5 ms before the clock's start.
SprjFlipper* constructed_flipper(Arena& a) {
    auto* f = a.zeroed<SprjFlipper>();
    f->vtable = reinterpret_cast<const void*>(bb::game::image().runtime(0x535a910));
    f->flip_mode = 4;
    f->flip_mode_heavy = 3;
    f->tearing = 1;
    f->target_frame_time = 1.0f / 30.0f;
    f->prev_time_us = f->now_time_us = clock_start - 5000;
    for (std::uint64_t& t : f->draw_time_us) {
        t = f->now_time_us;
    }
    f->foreground_skip_max_debug = -1;
    f->background_skip_max_debug = -1;
    f->no_skip = 1;
    return f;
}

struct PacerCase {
    std::string name;
    std::function<void(SprjFlipper&)> change;
    std::uint64_t step_us = 1000;
    std::uint64_t clock_us = clock_start;
    bool target_object = false;  // the object at 0x5540e00 that request_0x274 sets
};

// The window singleton the pacer asks whether it is in the foreground, the pacer, and the clock.
auto pacer_inputs(const PacerCase& c) {
    return [c](Arena& a) {
        start_clock(a, c.clock_us, c.step_us);
        global<FakeObject*>(0x5540500) = fake_object(a, 1);
        if (c.target_object) {
            global<void*>(0x5540e00) = a.zeroed<std::uint8_t>(0x60);
        }
        SprjFlipper* f = constructed_flipper(a);
        if (c.change) {
            c.change(*f);
        }
        return std::tuple<SprjFlipper*>{f};
    };
}

// Late frames: durations from the newest (at index 3) back, before them a frame on time.
void late_frames(SprjFlipper& f, const std::vector<std::uint64_t>& durations) {
    f.record_index = 3;
    std::uint32_t i = 3;
    for (const std::uint64_t d : durations) {
        f.records[i & 31] = {d, 1};
        --i;
    }
}

std::vector<PacerCase> pacer_cases() {
    std::vector<PacerCase> cases;
    cases.push_back({"constructed", {}});
    cases.push_back({"constructed, coarse clock", {}, 7919});
    cases.push_back({"constructed, a frame already over", {}, 50000});
    for (std::uint32_t mode : {0U, 1U, 2U, 3U, 4U, 5U, 0xffffffffU}) {
        for (std::uint8_t heavy : {0, 1}) {
            cases.push_back({"mode " + std::to_string(mode) + " heavy " + std::to_string(heavy),
                             [mode, heavy](SprjFlipper& f) {
                                 f.no_skip = 0;
                                 (heavy != 0 ? f.flip_mode_heavy : f.flip_mode) = mode;
                                 f.heavy = heavy;
                             }});
        }
    }
    cases.push_back({"forced default mode", [](SprjFlipper& f) {
                         f.flip_mode = 0;
                         f.force_default_mode = 1;
                     }});
    cases.push_back({"debug skip maxima", [](SprjFlipper& f) {
                         f.no_skip = 0;
                         f.foreground_skip_max_debug = 3;
                         f.background_skip_max_debug = 7;
                     }});
    cases.push_back({"clear skip once", [](SprjFlipper& f) {
                         f.no_skip = 0;
                         f.flip_mode = 3;
                         f.clear_skip_once = 1;
                     }});
    cases.push_back({"skip wait", [](SprjFlipper& f) { f.skip_wait = 1; }});
    cases.push_back({"clear skip once with debug maxima", [](SprjFlipper& f) {
                         f.no_skip = 0;
                         f.flip_mode = 3;
                         f.clear_skip_once = 1;
                         f.foreground_skip_max_debug = 3;
                         f.background_skip_max_debug = 7;
                     }});
    cases.push_back({"late with skip wait", [](SprjFlipper& f) {
                         f.no_skip = 0;
                         f.late = 1;
                         f.skip_wait = 1;
                         f.foreground_skip_max_debug = 3;
                         late_frames(f, {40000, 36000});
                     }});
    cases.push_back({"no skip and forced default mode", [](SprjFlipper& f) {
                         f.flip_mode = 0;
                         f.no_skip = 1;
                         f.force_default_mode = 1;
                         f.foreground_skip_max_debug = 2;
                     }});
    // Making up for late frames: the budget above the late frames, below them, and equal.
    const std::pair<const char*, std::vector<std::uint64_t>> late[] = {
        {"late, budget left", {40000, 36000}},
        {"late, budget used up", {60000, 50000, 70000}},
        {"late, budget exactly used", {33333 * 2, 33333}},
        {"late, little left", {33333, 33333, 33000}}};
    for (const auto& [name, durations] : late) {
        for (int debug_max : {1, 2, 3, 40}) {
            cases.push_back({std::string(name) + ", skip max " + std::to_string(debug_max),
                             [durations, debug_max](SprjFlipper& f) {
                                 f.no_skip = 0;
                                 f.late = 1;
                                 f.foreground_skip_max_debug = debug_max;
                                 late_frames(f, durations);
                             },
                             2000});
        }
    }
    cases.push_back({"late frame, made up late", [](SprjFlipper& f) {
                         f.no_skip = 0;
                         f.flip_mode = 3;
                         f.late = 1;
                         late_frames(f, {50000});
                     },
                     45000});
    cases.push_back({"clock behind the last frame", {}, 1000, clock_start - 1'000'000});
    cases.push_back({"clock far behind", [](SprjFlipper& f) { f.now_time_us = ~0ULL - 5; }});
    cases.push_back({"request without its object",
                     [](SprjFlipper& f) { f.request_0x274 = 1; }});
    cases.push_back({"request", [](SprjFlipper& f) { f.request_0x274 = 1; }, 1000, clock_start,
                     true});
    cases.push_back({"recent frame times", [](SprjFlipper& f) {
                         for (int i = 0; i < 16; ++i) {
                             f.recent_delta[i] = 0.03f + 0.001f * static_cast<float>(i);
                         }
                     }});
    cases.push_back({"recent frame times NaN and negative", [](SprjFlipper& f) {
                         f.recent_delta[3] = std::numeric_limits<float>::quiet_NaN();
                         f.recent_delta[7] = -0.5f;
                         f.recent_delta[0] = 1e30f;  // dropped
                     }});
    // Frame times the switch doesn't set (mode 5), with skip_wait so the wait doesn't run forever.
    for (float t : {0.0f, -0.25f, 1e30f, std::numeric_limits<float>::quiet_NaN(), 1e-9f}) {
        cases.push_back({"frame time " + std::to_string(t), [t](SprjFlipper& f) {
                             f.flip_mode = 5;
                             f.target_frame_time = t;
                             f.skip_wait = 1;
                         }});
    }
    return cases;
}

TEST_F(EbootTest, FlipperWaitFrameMatches) {
    EXPECT_EQ(image().function_size(flipper_wait_frame), std::optional<std::uint64_t>{0x84c});
    const FakeClockGuard clock;
    const FrameRateGuard rate(30);
    for (const PacerCase& c : pacer_cases()) {
        const Result r = harness().compare<void(SprjFlipper*)>(
            flipper_wait_frame, &bb::game::flipper_wait_frame, pacer_inputs(c));
        EXPECT_TRUE(r) << c.name << ": " << r.report();
    }
}

// At 60 FPS modes 3 and 4 and the forced mode take 1/60 s as their frame time; modes 0 and 1 keep
// 1/30 and mode 2 its own 1/60, each with its present interval and tearing.
TEST_F(EbootTest, FlipperWaitFrameAt60) {
    const FakeClockGuard clock;
    const FrameRateGuard rate(60);
    struct Expected {
        std::uint32_t mode;
        bool forced;
        float frame_time;
        std::int32_t interval;
        std::uint8_t tearing;
        std::uint64_t waited_us;
    };
    // The wait ends with the first read after the frame time since the last frame (5 ms before
    // the clock's start, reads 100 us apart). The forced mode doesn't wait.
    const Expected cases[] = {{4, false, 1.0f / 60.0f, 1, 0, 16700},
                              {3, false, 1.0f / 60.0f, 1, 0, 16700},
                              {0, true, 1.0f / 60.0f, 1, 0, 5000},
                              {0, false, 1.0f / 30.0f, 2, 1, 33400},
                              {1, false, 1.0f / 30.0f, 2, 1, 33400},
                              {2, false, 1.0f / 60.0f, 1, 1, 16700}};
    for (const Expected& e : cases) {
        harness().restore();
        SprjFlipper* f = std::get<0>(pacer_inputs({"60", {}, 100})(harness().arena()));
        f->flip_mode = e.mode;
        f->force_default_mode = e.forced ? 1 : 0;
        bb::game::flipper_wait_frame(f);
        SCOPED_TRACE(std::format("mode {}, forced {}", e.mode, e.forced));
        EXPECT_EQ(f->target_frame_time, e.frame_time);
        EXPECT_EQ(f->present_interval, e.interval);
        EXPECT_EQ(f->tearing, e.tearing);
        EXPECT_EQ(f->now_time_us - f->prev_time_us, e.waited_us);
        EXPECT_EQ(f->late, 0);
    }
}

// On the host clock the foreground wait sleeps until shortly before its end (#656): it still
// waits the whole frame time, but the thread spends only a small part of it on the CPU.
TEST_F(EbootTest, FlipperWaitFrameSleepsOnTheHostClock) {
    const FrameRateGuard rate(60);
    SprjFlipper* f = std::get<0>(pacer_inputs({"host clock", {}})(harness().arena()));
    bb::game::flipper_wait_frame(f);  // the fake start lies in the past: no wait
    const auto thread_cpu_ns = [] {
        timespec t{};
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
        return static_cast<std::int64_t>(t.tv_sec) * 1'000'000'000 + t.tv_nsec;
    };
    const std::int64_t cpu_before = thread_cpu_ns();
    const std::uint64_t first = f->now_time_us;
    constexpr int frames = 10;
    for (int i = 0; i < frames; ++i) {
        bb::game::flipper_wait_frame(f);
        EXPECT_GE(f->now_time_us - f->prev_time_us, 16666U) << "frame " << i;
    }
    const std::int64_t cpu_us = (thread_cpu_ns() - cpu_before) / 1000;
    const auto wall_us = static_cast<std::int64_t>(f->now_time_us - first);
    // Spinning all along costs the whole wall time; the margin is 2 ms of 16.7.
    EXPECT_LT(cpu_us, wall_us / 2) << "wall " << wall_us << " us";
}

int observed_frames = 0;

TEST_F(EbootTest, FlipperWaitFrameTellsTheObserver) {
    const FakeClockGuard clock;
    observed_frames = 0;
    const bb::game::FrameObserver observer = [] { ++observed_frames; };
    EXPECT_EQ(bb::game::set_frame_observer(observer), nullptr);
    SprjFlipper* f = std::get<0>(pacer_inputs({"observed", {}})(harness().arena()));
    bb::game::flipper_wait_frame(f);
    bb::game::flipper_wait_frame(f);
    EXPECT_EQ(bb::game::set_frame_observer(nullptr), observer);
    bb::game::flipper_wait_frame(f);
    EXPECT_EQ(observed_frames, 2);
}

TEST(FrameRate, RefusesOtherRates) {
    EXPECT_THROW(bb::game::set_frame_rate(0), std::invalid_argument);
    EXPECT_THROW(bb::game::set_frame_rate(90), std::invalid_argument);
    EXPECT_THROW(bb::game::set_frame_rate(144), std::invalid_argument);
    EXPECT_EQ(bb::game::frame_rate(), 30U);
    {
        const FrameRateGuard rate(60);
        EXPECT_EQ(bb::game::frame_step(), 1.0f / 60.0f);
    }
    {
        const FrameRateGuard rate(120);
        EXPECT_EQ(bb::game::frame_step(), 1.0f / 120.0f);
        EXPECT_EQ(bb::game::frame_rate_scale(), 4U);
        EXPECT_EQ(bb_frame_scale, 4U);
        EXPECT_EQ(bb_step_ratio, 0.25f);
        // Four steps of 1/120 are the game's 1/30 bit for bit, as two of 1/60 are.
        EXPECT_EQ(4.0f * bb::game::frame_step(), 1.0f / 30.0f);
    }
    EXPECT_EQ(bb::game::frame_rate(), 30U);
    std::uint32_t bits = 0;
    const float step = bb::game::frame_step();
    std::memcpy(&bits, &step, sizeof bits);
    EXPECT_EQ(bits, 0x3d088889U);  // the game's 1/30
}

// ---- the main loop tick -------------------------------------------------------------------------

struct TickCase {
    std::uint8_t window = 1;      // SprjWindow +0x18: the game goes on
    std::uint8_t quit = 0;        // the system object's +0x98
    int pending = -1;             // the object at 0x569fa30: none (-1), else its +0x28's result
    bool tasks_running = false;   // the task manager is inside a frame already
    std::uint64_t step_us = 1000;
};

struct TickObjects {
    FakeObject* window;
    FakeObject* system;
    FakeObject* line;
};
TickObjects tick_objects{};

// The singletons main_loop_tick uses, with the task manager's own fields where its frame reads
// them, a pacer that waits on the fake clock, and a task line that records the task data.
auto tick_inputs(const TickCase& c) {
    return [c](Arena& a) {
        start_clock(a, clock_start, c.step_us);
        tick_objects.window = fake_object(a, c.window);
        global<FakeObject*>(0x5540500) = tick_objects.window;
        SprjFlipper* f = constructed_flipper(a);
        f->no_skip = 0;
        global<SprjFlipper*>(0x55404f8) = f;
        global<void*>(0x5540510) = a.zeroed<std::uint64_t>(4);  // SprjTask
        auto* manager = a.zeroed<std::uint8_t>(0x60);
        void* empty = a.zeroed<std::uint64_t>(4);
        std::memcpy(manager + 0x10, &empty, 8);
        std::memcpy(manager + 0x18, &empty, 8);
        manager[0x38] = c.tasks_running ? 1 : 0;
        tick_objects.line = fake_object(a, 0);
        std::memcpy(manager + 0x40, &tick_objects.line, 8);
        const std::uint64_t a1 = 0x1111, a2 = 0x2222;
        std::memcpy(manager + 0x48, &a2, 8);
        std::memcpy(manager + 0x50, &a1, 8);
        global<void*>(0x54b2e30) = manager;  // FD4::FD4TaskManager
        if (c.pending >= 0) {
            global<FakeObject*>(0x569fa30) =
                fake_object(a, static_cast<std::uint8_t>(c.pending));
        }
        tick_objects.system = fake_object(a, c.quit);
        global<FakeObject*>(0x56a54f8) = tick_objects.system;
        return std::tuple<>{};
    };
}

TEST_F(EbootTest, MainLoopTickMatches) {
    EXPECT_EQ(image().function_size(main_loop_tick), std::optional<std::uint64_t>{0x1e0});
    const FakeClockGuard clock;
    const FrameRateGuard rate(30);
    std::vector<TickCase> cases;
    for (std::uint8_t window : {0, 1, 2, 0xff}) {
        for (std::uint8_t quit : {0, 1, 3}) {
            for (int pending : {-1, 0, 1}) {
                for (bool running : {false, true}) {
                    cases.push_back({window, quit, pending, running});
                }
            }
        }
    }
    cases.push_back({1, 0, -1, false, 50000});
    for (const TickCase& c : cases) {
        const Result r = harness().compare<std::uint8_t()>(main_loop_tick,
                                                           &bb::game::main_loop_tick,
                                                           tick_inputs(c));
        EXPECT_TRUE(r) << "window " << int{c.window} << " quit " << int{c.quit} << " pending "
                       << c.pending << " running " << c.tasks_running << ": " << r.report();
    }
}

// At 60 FPS every task gets a step of 1/60 s, and the global task data holds it.
TEST_F(EbootTest, MainLoopTickAt60) {
    const FakeClockGuard clock;
    const FrameRateGuard rate(60);
    tick_inputs({})(harness().arena());
    bb::game::set_frame_count(41);
    EXPECT_EQ(bb::game::main_loop_tick(), 1);
    EXPECT_EQ(bb::game::frame_count(), 42U);  // one more a tick
    std::uint32_t bits = 0;
    std::memcpy(&bits, &global<float>(0x54b7e08), sizeof bits);
    EXPECT_EQ(bits, 0x3c888889U);
    EXPECT_EQ(tick_objects.line->calls, 1U);
    EXPECT_EQ(tick_objects.line->delta_bits, 0x3c888889U);
    EXPECT_EQ(tick_objects.line->data_vtable,
              reinterpret_cast<const void*>(bb::game::image().runtime(0x52efd40)));
    EXPECT_EQ(tick_objects.line->arg1, 0x1111U);
    EXPECT_EQ(tick_objects.line->arg3, 0xffffffffU);
}

}  // namespace
