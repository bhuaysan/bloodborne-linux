// shadPS4's developer tools layer as an empty ImGui layer: the port has no developer tools.
#pragma once

#include "imgui/imgui_layer.h"

namespace Core::Devtools {

class Layer final : public ImGui::Layer {
public:
    static void SetupSettings() {}
    static void DrawNullGpuNotice() {}

    void Draw() override {}
    bool ShouldKeepDrawing() override { return false; }
};

}  // namespace Core::Devtools
