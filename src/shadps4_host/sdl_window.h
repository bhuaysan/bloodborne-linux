// The interface video_core expects from shadPS4's SDL window (sdl_window.h of v.0.21.0), declared
// only. #432 implements it over the SDL3 window bb-run opens (docs/design/renderer.md, "The window
// is ours, the surface is upstream's"). Until then GetWindowInfo reports a headless window, with
// which the instance creates its device without a surface, and the other members end the process
// when called (video_core_host.cpp).
#pragma once

#include "common/types.h"

struct SDL_Window;

namespace Frontend {

enum class WindowSystemType : u8 { Headless, Windows, X11, Wayland, Metal };

struct WindowSystemInfo {
    // The display connection on X11 and Wayland.
    void* display_connection = nullptr;
    // The native window: an X11 Window or a wl_surface*.
    void* render_surface = nullptr;
    // >1 on high-DPI displays.
    float render_surface_scale = 1.0f;
    WindowSystemType type = WindowSystemType::Headless;
};

class WindowSDL {
public:
    s32 GetWidth() const;
    s32 GetHeight() const;
    SDL_Window* GetSDLWindow() const;
    WindowSystemInfo GetWindowInfo() const;
};

}  // namespace Frontend
