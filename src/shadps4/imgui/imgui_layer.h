// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/imgui/imgui_layer.h @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-09
// License texts: third_party/shadps4/LICENSES/

#pragma once

namespace ImGui {

class Layer {
public:
    virtual ~Layer() = default;
    static void AddLayer(Layer* layer);
    static void RemoveLayer(Layer* layer);

    virtual void Draw() = 0;
    virtual bool ShouldKeepDrawing() {
        return true;
    }
};

} // namespace ImGui