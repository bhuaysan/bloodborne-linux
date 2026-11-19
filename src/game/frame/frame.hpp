// The main loop's frame pacing and time step, and bb-run's target frame rate
// (docs/design/frame-rate.md). Layouts as in symbols/types/frame.h.
#pragma once

#include <cstddef>
#include <cstdint>

namespace bb::game {

// ---- the target frame rate ---------------------------------------------------------------------

// The frame rates bb-run offers: the game's own 30 and 60.
inline constexpr unsigned default_frame_rate = 30;

// Sets the target frame rate (30, 60 or 120; anything else throws std::invalid_argument). Called
// before the game runs; not thread-safe.
void set_frame_rate(unsigned fps);
unsigned frame_rate();

// Frames per 30 FPS frame, frame_rate() / default_frame_rate: 1 at 30, 2 at 60, 4 at 120.
inline unsigned frame_rate_scale() {
    return frame_rate() / default_frame_rate;
}

// 1 / frame_rate() seconds as a float: the pacer's frame time and the main loop's time step. At
// 30 it is the game's own constant 0x3d088889.
float frame_step();

// The number of main loop iterations so far (main_loop_tick adds 1 before it runs the tasks): a
// frame count that no game state holds back, unlike the game's own counters. Read and written on
// the game's main thread. set_frame_count is for tests.
std::uint32_t frame_count();
void set_frame_count(std::uint32_t count);

// Called at the start of every frame pacer call, on the game's main thread (bb-run's main-loop
// probe). Returns the observer it replaces; nullptr removes it.
using FrameObserver = void (*)();
FrameObserver set_frame_observer(FrameObserver observer);

// ---- the game's types --------------------------------------------------------------------------

// The task data the main loop hands to every task: a vtable and the time step in seconds.
struct FD4TaskData {
    const void* vtable;
    float delta_time;
};
static_assert(sizeof(FD4TaskData) == 0x10 && offsetof(FD4TaskData, delta_time) == 8);

struct FlipperFrameRecord {
    std::uint64_t duration_us;
    std::uint8_t late;
};
static_assert(sizeof(FlipperFrameRecord) == 0x10);

// The frame pacer, a singleton of 0x2c8 bytes.
struct SprjFlipper {
    const void* vtable;
    std::uint32_t flip_mode;        // 0 to 4, see docs/design/frame-rate.md
    std::uint32_t flip_mode_heavy;  // while `heavy` was set
    std::int32_t present_interval;
    std::uint8_t tearing;
    float target_frame_time;  // seconds
    std::uint64_t prev_time_us;
    std::uint64_t now_time_us;
    std::uint64_t draw_time_us[6];
    FlipperFrameRecord records[32];
    std::uint32_t record_index;
    float delta_time;  // the last frame's duration in seconds, not read by the game
    std::uint32_t foreground_skip_max;
    std::uint32_t background_skip_max;
    std::uint8_t late;
    std::uint8_t clear_skip_once;
    std::uint8_t no_skip;
    std::uint8_t skip_wait;
    std::uint8_t request_0x274;
    std::uint8_t heavy_last;
    std::uint8_t heavy;
    float recent_delta[16];
    float fps;
    std::int32_t foreground_skip_max_debug;
    std::int32_t background_skip_max_debug;
    std::uint8_t force_default_mode;
    std::uint8_t draw_time_live;
};
static_assert(offsetof(SprjFlipper, tearing) == 0x14 &&
              offsetof(SprjFlipper, target_frame_time) == 0x18 &&
              offsetof(SprjFlipper, records) == 0x60 &&
              offsetof(SprjFlipper, record_index) == 0x260 &&
              offsetof(SprjFlipper, late) == 0x270 &&
              offsetof(SprjFlipper, heavy) == 0x276 &&
              offsetof(SprjFlipper, recent_delta) == 0x278 && offsetof(SprjFlipper, fps) == 0x2b8 &&
              offsetof(SprjFlipper, force_default_mode) == 0x2c4 && sizeof(SprjFlipper) == 0x2c8);

// ---- the replaced functions --------------------------------------------------------------------

// 0x02034770: waits until the frame time of the current flip mode has passed since the previous
// frame, records the frame, and sets the present interval. Modes 3 and 4 (the default and the
// online one) and the forced default mode take frame_step() as their frame time.
void flipper_wait_frame(SprjFlipper* self);

// 0x02018d20: one main loop iteration. Creates the pacer on the first call, waits for the frame,
// runs every task with a time step of frame_step(), and returns whether the game goes on: the
// window's answer and not the system's quit request, bytewise as the game computes it.
std::uint8_t main_loop_tick();

}  // namespace bb::game

// frame_step()'s value, for the stubs patched into game code (BB_PATCH), which load it from memory.
// Written only by set_frame_rate.
// bb_step_ratio is 30 / frame_rate(), exactly 1 at 30, 0.5 at 60 and 0.25 at 120: the share of a 30 FPS frame
// that a frame stands for. bb_frame_scale is frame_rate_scale(), 1 at 30, 2 at 60 and 4 at 120.
extern "C" {
extern float bb_frame_step;
extern float bb_step_ratio;
extern std::uint32_t bb_frame_scale;
}
