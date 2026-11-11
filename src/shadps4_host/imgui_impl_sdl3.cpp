// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/imgui/renderer/imgui_impl_sdl3.cpp @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-11: Init, Shutdown, the time step of NewFrame and OnResize
// only; no input, clipboard, IME, cursors, gamepads or frame rate statistics. Shutdown without a
// backend does nothing.
// License texts: third_party/shadps4/LICENSES/

// Based on imgui_impl_sdl3.cpp from Dear ImGui repository

#include <imgui.h>

#include <SDL3/SDL.h>

#include "imgui_impl_sdl3.h"

namespace ImGui::Sdl {

struct SdlData {
    SDL_Window* window{};
    Uint64 time{};
};

static SdlData* GetBackendData() {
    return ImGui::GetCurrentContext() ? (SdlData*)ImGui::GetIO().BackendPlatformUserData : nullptr;
}

bool Init(SDL_Window* window) {
    ImGuiIO& io = ImGui::GetIO();
    IMGUI_CHECKVERSION();
    IM_ASSERT(io.BackendPlatformUserData == nullptr && "Already initialized a platform backend!");

    SdlData* bd = IM_NEW(SdlData)();
    io.BackendPlatformUserData = (void*)bd;
    io.BackendPlatformName = "imgui_impl_sdl3_shadps4";
    bd->window = window;
    return true;
}

void Shutdown() {
    SdlData* bd = GetBackendData();
    if (bd == nullptr) {
        return;  // a renderer without a window (tests/shadps4/video_core_test.cpp) never had one
    }
    ImGuiIO& io = ImGui::GetIO();
    io.BackendPlatformName = nullptr;
    io.BackendPlatformUserData = nullptr;
    IM_DELETE(bd);
}

void NewFrame(bool) {
    SdlData* bd = GetBackendData();
    IM_ASSERT(bd != nullptr && "No platform backend to shutdown, or already shutdown?");
    ImGuiIO& io = ImGui::GetIO();

    // Setup time step (we don't use SDL_GetTicks() because it is using millisecond resolution)
    // (Accept SDL_GetPerformanceCounter() not returning a monotonically increasing value. Happens
    // in VMs and Emscripten, see #6189, #6114, #3644)
    static Uint64 frequency = SDL_GetPerformanceFrequency();
    Uint64 current_time = SDL_GetPerformanceCounter();
    if (current_time <= bd->time)
        current_time = bd->time + 1;
    io.DeltaTime = bd->time > 0 ? (float)((double)(current_time - bd->time) / (double)frequency)
                                : 1.0f / 60.0f;
    bd->time = current_time;
}

void OnResize() {
    SdlData* bd = GetBackendData();
    ImGuiIO& io = ImGui::GetIO();

    int w, h;
    int display_w, display_h;
    SDL_GetWindowSize(bd->window, &w, &h);
    if (SDL_GetWindowFlags(bd->window) & SDL_WINDOW_MINIMIZED) {
        w = h = 0;
    }
    SDL_GetWindowSizeInPixels(bd->window, &display_w, &display_h);
    io.DisplaySize = ImVec2((float)w, (float)h);
    if (w > 0 && h > 0) {
        io.DisplayFramebufferScale = {(float)display_w / (float)w, (float)display_h / (float)h};
    }
}

}  // namespace ImGui::Sdl
