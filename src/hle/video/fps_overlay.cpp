#include "hle/video/fps_overlay.hpp"

#include <algorithm>

#include <imgui.h>

namespace bb::hle::video {

FpsOverlay::~FpsOverlay() {
    set_shown(false);
}

void FpsOverlay::frame() {
    std::scoped_lock lock{mutex_};
    meter_.frame(common::FpsMeter::Clock::now());
}

void FpsOverlay::set_shown(bool shown) {
    if (shown_.exchange(shown) == shown) {
        return;
    }
    if (shown) {
        AddLayer(this);
    } else {
        RemoveLayer(this);
    }
}

void FpsOverlay::Draw() {
    common::FpsMeter::Reading r;
    {
        std::scoped_lock lock{mutex_};
        meter_.update(common::FpsMeter::Clock::now());
        r = meter_.reading();
    }
    const ImGuiIO& io = ImGui::GetIO();
    // ImGui's base font is 16 px: 18 px at 1080p, growing with the window and never under 12 px.
    // Small and tight in the corner, so it stays above the game's health bar.
    const float scale = std::max(0.75f, 1.1f * io.DisplaySize.y / 1080.0f);
    const float margin = 2.0f * scale;
    ImGui::SetNextWindowPos(ImVec2(margin, margin));
    ImGui::SetNextWindowBgAlpha(0.35f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f * scale, 1.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin("##bb_fps", nullptr, flags)) {
        ImGui::SetWindowFontScale(scale);
        if (r.valid) {
            ImGui::Text("%3.0f FPS  %5.1f ms  max %5.1f ms", r.fps, r.frame_ms, r.worst_ms);
        } else {
            ImGui::TextUnformatted("  - FPS");
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

}  // namespace bb::hle::video
