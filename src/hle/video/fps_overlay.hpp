// The in-game frame rate counter (bb-run --show-fps, config key show_fps, F10 in the window): an
// ImGui layer the Presenter draws on top of the presented frame, as it draws the IME dialog. It
// counts the game's own frames, not the repeats of the last frame the present thread shows while
// the game is busy, so it reads what the game renders.
#pragma once

#include <atomic>
#include <mutex>

#include "common/fps_meter.hpp"
#include "imgui/imgui_layer.h"

namespace bb::hle::video {

class FpsOverlay final : public ImGui::Layer {
public:
    ~FpsOverlay() override;

    // A game frame was presented. Any thread.
    void frame();

    // Shows or hides the counter, from any thread; it changes between two presents.
    void set_shown(bool shown);
    void toggle() { set_shown(!shown_.load(std::memory_order_relaxed)); }
    bool shown() const { return shown_.load(std::memory_order_relaxed); }

    void Draw() override;
    // Only draws along with frames that are presented anyway: no presents of its own.
    bool ShouldKeepDrawing() override { return false; }

private:
    std::mutex mutex_;  // the meter: frames come from the GPU thread, Draw from a present
    common::FpsMeter meter_;
    std::atomic<bool> shown_{false};
};

}  // namespace bb::hle::video
