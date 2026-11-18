// The main loop's tick and the target frame rate (docs/design/frame-rate.md, "The main loop").
#include <cstdint>
#include <format>
#include <stdexcept>

#include "game/fd4.hpp"
#include "game/frame/frame.hpp"
#include "game/image.hpp"
#include "hook/hook.hpp"

extern "C" {
float bb_frame_step = 1.0f / static_cast<float>(bb::game::default_frame_rate);
float bb_step_ratio = 1.0f;
}

namespace bb::game {
namespace {

unsigned target_fps = default_frame_rate;
std::uint32_t frames = 0;

}  // namespace

void set_frame_rate(unsigned fps) {
    if (fps != 30 && fps != 60) {
        throw std::invalid_argument(std::format("frame rate {} is not 30 or 60", fps));
    }
    target_fps = fps;
    bb_frame_step = 1.0f / static_cast<float>(fps);
    bb_step_ratio = static_cast<float>(default_frame_rate) / static_cast<float>(fps);
}

unsigned frame_rate() {
    return target_fps;
}

float frame_step() {
    return bb_frame_step;
}

std::uint32_t frame_count() {
    return frames;
}

void set_frame_count(std::uint32_t count) {
    frames = count;
}

// 0x02018d20
std::uint8_t main_loop_tick() {
    void*& window = global<void*>(0x05540500);
    if (window == nullptr) {
        singleton_missing(0x0493b00c);  // "SprjWindow"
    }
    const auto window_open = vcall<std::uint8_t>(window, 0x18);

    SprjFlipper*& flipper = global<SprjFlipper*>(0x055404f8);
    if (flipper == nullptr) {
        void* const allocator = global<void*>(0x05540408);
        auto* const created = vcall<SprjFlipper*>(
            allocator, 0x58, std::uint64_t{sizeof(SprjFlipper)}, std::uint64_t{8});
        if (created != nullptr) {
            fn<void(SprjFlipper*)>(0x02034520)(created);  // flipper_construct
            flipper = created;
        } else {
            flipper = nullptr;
            singleton_missing(0x0493acac);  // "SprjFlipper"
        }
        flipper->no_skip = 1;
        if (flipper == nullptr) {
            singleton_missing(0x0493acac);
        }
    }
    fn<void(SprjFlipper*)>(0x02034770)(flipper);  // flipper_wait_frame
    ++frames;

    FD4TaskData data{reinterpret_cast<const void*>(image().runtime(0x052efd40)), frame_step()};
    if (global<void*>(0x05540510) == nullptr) {
        singleton_missing(0x0493b288);  // "SprjTask"
    }
    fn<void(void*, FD4TaskData*)>(0x020512a0)(nullptr, &data);  // task_manager_run_frame

    // A use of the window singleton whose result the game drops: only its check remains.
    void* const pending = global<void*>(0x0569fa30);
    if ((pending == nullptr || vcall<std::uint8_t>(pending, 0x28) != 0) && window == nullptr) {
        singleton_missing(0x0493b00c);
    }
    void* system = global<void*>(0x056a54f8);
    if (system == nullptr) {
        system = fn<void*()>(0x020e2550)();
    }
    const auto quit = vcall<std::uint8_t>(system, 0x98);
    return window_open & (quit ^ 1);
}

}  // namespace bb::game

BB_REPLACE(0x02018d20, bb::game::main_loop_tick);
