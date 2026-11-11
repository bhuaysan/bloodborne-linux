// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/sdl_window.cpp @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-11: the window creation and the native handles of the
// WindowSDL constructor, and OnResize; X11 and Wayland only. Open and PumpEvents are ours: no
// input, gamepads, camera, audio, fullscreen or hotkeys.
// License texts: third_party/shadps4/LICENSES/

#include "sdl_window.h"

#include <SDL3/SDL.h>
#include <imgui.h>

#include "imgui/renderer/imgui_core.h"

namespace Frontend {

std::unique_ptr<WindowSDL> WindowSDL::Open(std::string_view title, s32 width_, s32 height_,
                                           std::string& why) {
    // SDL would otherwise turn SIGINT and SIGTERM into SDL_EVENT_QUIT, so Ctrl-C would not end the
    // run.
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        why = SDL_GetError();
        return nullptr;
    }
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, std::string(title).c_str());
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width_);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height_);
    SDL_SetNumberProperty(props, "flags", SDL_WINDOW_VULKAN);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_Window* const sdl_window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (sdl_window == nullptr) {
        why = SDL_GetError();
        SDL_Quit();
        return nullptr;
    }

    auto result = std::unique_ptr<WindowSDL>(new WindowSDL);
    result->window = sdl_window;
    SDL_SetWindowMinimumSize(sdl_window, 640, 360);
    SDL_SyncWindow(sdl_window);
    int w = 0;
    int h = 0;
    SDL_GetWindowSizeInPixels(sdl_window, &w, &h);
    result->width = w;
    result->height = h;

    WindowSystemInfo& info = result->window_info;
    const SDL_PropertiesID window_props = SDL_GetWindowProperties(sdl_window);
    if (SDL_strcmp(SDL_GetCurrentVideoDriver(), "x11") == 0) {
        info.type = WindowSystemType::X11;
        info.display_connection =
            SDL_GetPointerProperty(window_props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, NULL);
        info.render_surface = (void*)SDL_GetNumberProperty(
            window_props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
    } else if (SDL_strcmp(SDL_GetCurrentVideoDriver(), "wayland") == 0) {
        info.type = WindowSystemType::Wayland;
        info.display_connection =
            SDL_GetPointerProperty(window_props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, NULL);
        info.render_surface =
            SDL_GetPointerProperty(window_props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, NULL);
    } else {
        why = std::string("unsupported SDL video driver ") + SDL_GetCurrentVideoDriver();
        return nullptr;  // the destructor closes the window and SDL
    }
    return result;
}

WindowSDL::~WindowSDL() {
    if (window != nullptr) {
        SDL_DestroyWindow(window);
        SDL_Quit();
    }
}

bool WindowSDL::PumpEvents(s32 timeout_ms) {
    SDL_Event event;
    for (bool got = SDL_WaitEventTimeout(&event, timeout_ms); got; got = SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            return false;
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_MAXIMIZED:
        case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_MINIMIZED:
        case SDL_EVENT_WINDOW_EXPOSED:
            OnResize();
            break;
        default:
            break;
        }
    }
    return true;
}

void WindowSDL::OnResize() {
    int w = 0;
    int h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    width = w;
    height = h;
    // Before the presenter exists there is no ImGui context to resize.
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGui::Core::OnResize();
    }
}

}  // namespace Frontend
