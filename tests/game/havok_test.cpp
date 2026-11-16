// Differential tests of the Havok world step in src/game/havok/, and of what it does at 60 FPS.
// The step's callees (the collide and solve stages, the job queue's processing, the manager's other
// per-frame calls) are hooked with fakes that record their arguments, so the tests compare the step
// record the stages get without a Havok world; the stages themselves stay the game's code in
// bb-run.
#include <array>
#include <cstdint>
#include <cstring>
#include <tuple>

#include <gtest/gtest.h>

#include "game/frame/frame.hpp"
#include "game/havok/world.hpp"
#include "game/image.hpp"
#include "harness/eboot.hpp"
#include "harness/harness.hpp"
#include "hle/hle.hpp"
#include "hook/hook.hpp"

namespace {

using bb::game::HavokStepInfo;
using bb::harness::Arena;
using bb::harness::Result;
using bb::test::EbootTest;

constexpr std::uint64_t havok_world_step = 0x17f99d0;
constexpr std::uint64_t counter_singleton = 0x553b168;

// ---- the call log -------------------------------------------------------------------------------

enum What : std::uint64_t {
    before_call = 1,  // 0x17fd220
    after_counter,    // 0x1806e90
    counter_item,     // 0x15a5fc0
    held_call,        // 0xb50410
    pre_collide,      // 0x1e42140
    collide,          // 0xb5a2e0
    solve,            // 0xb5b100
    finish,           // 0x43b0a0
    pool_threads,     // thread pool +0x48
    pool_process,     // thread pool +0x30
};

struct Call {
    std::uint64_t what;
    std::uint64_t args[6];
};

// In the arena, so the harness compares it between the original and the replacement.
struct Log {
    std::uint32_t count;
    Call calls[64];
};
Log* log_ = nullptr;

// What a fake changes in the manager while the step runs, to pin down when the step reads its
// fields: the original reads +0x30 once before 0x17fd220, the job queue (+0x68) after the thread
// pool's +0x48 and +0x28 again after the counter's items.
enum class Mutation { none, held_by_before, queue_by_threads, holder_by_item };
Mutation mutation_ = Mutation::none;
std::byte* manager_ = nullptr;
void* other_ = nullptr;  // the queue or holder a mutation puts in

void record(What what, std::uint64_t a = 0, std::uint64_t b = 0, std::uint64_t c = 0,
            std::uint64_t d = 0, std::uint64_t e = 0, std::uint64_t f = 0) {
    ASSERT_LT(log_->count, std::size(log_->calls));
    log_->calls[log_->count++] = {what, {a, b, c, d, e, f}};
}

std::uint64_t u(const void* p) {
    return reinterpret_cast<std::uint64_t>(p);
}

void fake_before(void* p) {
    record(before_call, u(p));
    if (mutation_ == Mutation::held_by_before) {
        manager_[0x30] = static_cast<std::byte>(1);
    }
}
void fake_after_counter(void* p) {
    record(after_counter, u(p));
}
void fake_counter_item(void* p, std::int32_t n) {
    record(counter_item, u(p), static_cast<std::uint32_t>(n));
    if (mutation_ == Mutation::holder_by_item) {
        std::memcpy(manager_ + 0x28, &other_, sizeof other_);
    }
}
void fake_held(void* p) {
    record(held_call, u(p));
}
void fake_pre_collide(void* p) {
    record(pre_collide, u(p));
}
// The stage passes the solve stage something through `out`.
void fake_collide(void* world, const HavokStepInfo* info, void* queue, std::uint64_t* out) {
    std::uint32_t delta_bits = 0;
    std::memcpy(&delta_bits, &info->delta_time, sizeof delta_bits);
    record(collide, u(world), delta_bits, info->manager_c8,
           static_cast<std::uint32_t>(info->threads), static_cast<std::uint32_t>(info->zero),
           u(queue));
    *out = 0x5eed0000 + log_->count;
}
void fake_solve(void* world, const std::uint64_t* stage, void* queue) {
    record(solve, u(world), *stage, u(queue));
}
void fake_finish(void* queue) {
    record(finish, u(queue));
}
std::int32_t fake_pool_threads(void* pool) {
    record(pool_threads, u(pool));
    if (mutation_ == Mutation::queue_by_threads) {
        std::memcpy(manager_ + 0x68, &other_, sizeof other_);
    }
    return 3;
}
void fake_pool_process(void* pool, void* queue, std::uint32_t type) {
    record(pool_process, u(pool), u(queue), type);
}

// Hooks the step's callees with the fakes for one test.
class StageFakes {
public:
    StageFakes() { bb::hook::apply(bb::game::image(), fakes_); }
    ~StageFakes() { bb::hook::remove(bb::game::image(), fakes_); }
    StageFakes(const StageFakes&) = delete;
    StageFakes& operator=(const StageFakes&) = delete;

private:
    template <class F>
    static bb::hook::Replacement fake(std::uint64_t link, F* f, const char* name) {
        return {link, reinterpret_cast<void*>(f), name};
    }
    const std::array<bb::hook::Replacement, 8> fakes_{
        fake(0x17fd220, &fake_before, "fake_before"),
        fake(0x1806e90, &fake_after_counter, "fake_after_counter"),
        fake(0x15a5fc0, &fake_counter_item, "fake_counter_item"),
        fake(0xb50410, &fake_held, "fake_held"),
        fake(0x1e42140, &fake_pre_collide, "fake_pre_collide"),
        fake(0xb5a2e0, &fake_collide, "fake_collide"),
        fake(0xb5b100, &fake_solve, "fake_solve"),
        fake(0x43b0a0, &fake_finish, "fake_finish"),
    };
};

// Sets the frame rate for one test and puts back the default, 30, also when the test fails early.
class FrameRateGuard {
public:
    explicit FrameRateGuard(unsigned fps) { bb::game::set_frame_rate(fps); }
    ~FrameRateGuard() { bb::game::set_frame_rate(bb::game::default_frame_rate); }
    FrameRateGuard(const FrameRateGuard&) = delete;
    FrameRateGuard& operator=(const FrameRateGuard&) = delete;
};

// A guest mutex made once and shared by every run, so its cell holds the same value each time.
std::uint64_t shared_mutex() {
    static const std::uint64_t cell = [] {
        static std::uint64_t c = 0;
        using MutexCall = int(void*);
        auto* lock = reinterpret_cast<MutexCall*>(
            bb::hle::import_address("libScePosix", "7H0iTOciTLo"));
        auto* unlock = reinterpret_cast<MutexCall*>(
            bb::hle::import_address("libScePosix", "2Z+PpY6CaJg"));
        EXPECT_EQ(lock(&c), 0);
        EXPECT_EQ(unlock(&c), 0);
        return c;
    }();
    return cell;
}

// ---- the manager --------------------------------------------------------------------------------

template <class T>
T& at(void* object, std::size_t offset) {
    return *reinterpret_cast<T*>(static_cast<std::byte*>(object) + offset);
}

struct Case {
    const char* name;
    std::uint8_t held = 0;      // +0x30
    std::uint8_t stepping = 1;  // +0x26c
    bool pool = true;           // +0x70
    bool holder = true;         // +0x28
    bool before = true;         // +0x260
    std::int32_t spins = 3;     // the job queue's critical section
    bool counter = false;       // the singleton at 0x553b168
    std::int32_t count = 0;     // its +0x30
    std::int32_t last = 0;      // its +0x38
    std::uint8_t queue_flag = 0;
    Mutation mutation = Mutation::none;
};

const Case cases[] = {
    {.name = "step"},
    {.name = "step, lock without spinning", .spins = 0},
    {.name = "step, queue already marked", .queue_flag = 1},
    {.name = "step, nothing before", .before = false},
    {.name = "held", .held = 1},
    {.name = "held, no world", .held = 1, .holder = false},
    {.name = "not stepping", .stepping = 0},
    {.name = "no pool", .pool = false},
    {.name = "no world", .holder = false},
    {.name = "counter below 200", .counter = true, .count = 150, .last = 7},
    {.name = "counter 450", .counter = true, .count = 450},
    {.name = "counter 450, unchanged", .counter = true, .count = 450, .last = 2},
    {.name = "counter -450", .counter = true, .count = -450},
    {.name = "counter 450, held", .held = 1, .counter = true, .count = 450},
    {.name = "held set by the call before", .mutation = Mutation::held_by_before},
    {.name = "queue swapped by the thread count", .mutation = Mutation::queue_by_threads},
    {.name = "world swapped by the counter's items", .counter = true, .count = 450,
     .mutation = Mutation::holder_by_item},
};

std::tuple<void*> build_manager(Arena& a, const Case& c) {
    log_ = a.zeroed<Log>();
    auto* m = static_cast<std::byte*>(a.alloc(0x280));
    manager_ = m;
    mutation_ = c.mutation;
    at<std::uint8_t>(m, 0x30) = c.held;
    at<std::uint8_t>(m, 0x26c) = c.stepping;
    at<void*>(m, 0x20) = a.alloc(0x40);
    at<std::uint64_t>(m, 0xc8) = 0x1234'5678'9abc'def0;
    if (c.before) {
        at<void*>(m, 0x260) = a.alloc(0x40);
    }
    if (c.holder) {
        void* holder = a.alloc(0x40);
        at<void*>(holder, 8) = a.alloc(0x100);  // the world
        at<void*>(m, 0x28) = holder;
    }
    auto* queue = static_cast<std::byte*>(a.alloc(0x100));
    at<std::uint64_t>(queue, 0x88) = shared_mutex();
    at<std::int32_t>(queue, 0x90) = c.spins;
    at<std::uint8_t>(queue, 0xc9) = c.queue_flag;
    at<void*>(m, 0x68) = queue;
    if (c.mutation == Mutation::queue_by_threads) {
        auto* other = static_cast<std::byte*>(a.alloc(0x100));
        at<std::uint64_t>(other, 0x88) = shared_mutex();
        at<std::int32_t>(other, 0x90) = c.spins;
        other_ = other;
    } else if (c.mutation == Mutation::holder_by_item) {
        void* holder = a.alloc(0x40);
        at<void*>(holder, 8) = a.alloc(0x100);
        other_ = holder;
    }
    if (c.pool) {
        auto** vtable = a.zeroed<void*>(16);
        vtable[0x30 / 8] = reinterpret_cast<void*>(&fake_pool_process);
        vtable[0x48 / 8] = reinterpret_cast<void*>(&fake_pool_threads);
        auto** pool = a.zeroed<void*>(4);
        pool[0] = vtable;
        at<void*>(m, 0x70) = pool;
    }
    void*& counter = bb::game::global<void*>(counter_singleton);
    counter = nullptr;
    if (c.counter) {
        auto* s = static_cast<std::byte*>(a.alloc(0x40));
        at<std::int32_t>(s, 0x30) = c.count;
        at<std::int32_t>(s, 0x38) = c.last;
        at<std::int32_t>(s, 0x20) = 3;  // entries with 2, 0 and 1 items
        auto* entries = static_cast<std::byte*>(a.alloc(3 * 0x1f8));
        const std::int32_t items[3] = {2, 0, 1};
        for (int i = 0; i < 3; ++i) {
            at<std::int32_t>(entries + i * 0x1f8, 0xc8) = items[i];
            at<void*>(entries + i * 0x1f8, 0xd0) = a.alloc(2 * 0x2e0);
        }
        at<void*>(s, 0x28) = entries;
        counter = s;
    }
    return {m};
}

// The calls the case makes, by kind, to show that the fakes ran.
std::uint32_t calls_of(What what) {
    std::uint32_t n = 0;
    for (std::uint32_t i = 0; i < log_->count; ++i) {
        n += log_->calls[i].what == what ? 1 : 0;
    }
    return n;
}

std::uint32_t float_bits(float f) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof bits);
    return bits;
}

TEST_F(EbootTest, HavokWorldStepMatches) {
    const StageFakes fakes;
    for (const Case& c : cases) {
        const Result r = harness().compare<void(void*)>(
            havok_world_step, &bb::game::havok_world_step,
            [&](Arena& a) { return build_manager(a, c); });
        EXPECT_TRUE(r) << c.name << ": " << r.report();
        const bool steps = c.held == 0 && c.stepping != 0 && c.pool && c.holder;
        EXPECT_EQ(calls_of(collide), steps ? 1u : 0u) << c.name;
        EXPECT_EQ(calls_of(solve), steps ? 1u : 0u) << c.name;
        EXPECT_EQ(calls_of(held_call), c.held != 0 && c.holder ? 1u : 0u) << c.name;
    }
    // The counter's items: 450 hands 2 to each of the 3 items, the others hand out nothing.
    harness().restore();
    void* const m = std::get<0>(build_manager(harness().arena(), cases[10]));
    bb::game::havok_world_step(m);
    EXPECT_EQ(calls_of(counter_item), 3u);
    harness().restore();
}

TEST_F(EbootTest, HavokWorldStepAt60) {
    const StageFakes fakes;
    const auto collide_delta = [&](bool replacement) {
        harness().restore();
        void* const m = std::get<0>(build_manager(harness().arena(), cases[0]));
        if (replacement) {
            bb::game::havok_world_step(m);
        } else {
            bb::game::fn<void(void*)>(havok_world_step)(m);
        }
        EXPECT_EQ(calls_of(collide), 1u);
        for (std::uint32_t i = 0; i < log_->count; ++i) {
            if (log_->calls[i].what == collide) {
                return static_cast<std::uint32_t>(log_->calls[i].args[1]);
            }
        }
        return 0u;
    };
    EXPECT_EQ(collide_delta(false), 0x3d088889u);  // the game's 1/30
    {
        const FrameRateGuard rate(60);
        EXPECT_EQ(collide_delta(false), 0x3d088889u);
        EXPECT_EQ(collide_delta(true), float_bits(1.0f / 60.0f));
    }
    EXPECT_EQ(collide_delta(true), 0x3d088889u);
    harness().restore();
}

}  // namespace
