// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/sdl_window.h @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-11: the window and surface part only, over the SDL3
// window bb-run opens (docs/design/renderer.md, "The window is ours, the surface is upstream's").
// No input of its own, settings or fullscreen; Open and PumpEvents are ours. 2026-11-21: Open
// takes the display to open on. 2026-11-22: PumpEvents ends the run on the controller's quit chord;
// SyncTextInput (#795). 2026-11-24: F10 toggles the frame rate counter.
// License texts: third_party/shadps4/LICENSES/
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <string_view>

#include "common/quit_chord.hpp"
#include "common/types.h"

struct SDL_Window;
union SDL_Event;

namespace Frontend {

enum class WindowSystemType : u8 { Headless, X11, Wayland };

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
    // A headless window: no SDL window behind it, so the instance creates its device without a
    // surface. Its size is 0 x 0.
    WindowSDL() = default;
    // Initialises SDL's video and opens a resizable Vulkan window of `width` x `height` on X11 or
    // Wayland; `fullscreen` makes it a borderless window covering its display, at the display's
    // size. The window is centred on the display whose name contains `display`, ignoring case,
    // else on the primary display; `display_note` then says why (empty when nothing was asked or
    // it matched). Nothing when SDL can't; `why` then gives SDL's error.
    static std::unique_ptr<WindowSDL> Open(std::string_view title, s32 width, s32 height,
                                           bool fullscreen, std::string_view display,
                                           std::string& display_note, std::string& why);
    ~WindowSDL();
    WindowSDL(const WindowSDL&) = delete;
    WindowSDL& operator=(const WindowSDL&) = delete;

    // The size in pixels, as of the last resize event. The present thread reads it.
    s32 GetWidth() const { return width; }
    s32 GetHeight() const { return height; }
    SDL_Window* GetSDLWindow() const { return window; }
    // The name of the display the window is on, as SDL has it; empty when SDL doesn't know.
    std::string DisplayName() const;
    WindowSystemInfo GetWindowInfo() const { return window_info; }

    // Handles the pending events on the thread that opened the window, after waiting up to
    // `timeout_ms` for the first. Returns false once the window is to close, or once Back and Start
    // have been held together long enough (common/quit_chord.hpp).
    bool PumpEvents(s32 timeout_ms);

    // Where PumpEvents hands the gamepad events (device added and removed, buttons, axes,
    // touchpad, sensors): the pad lives in a library the window does not link.
    using EventHandler = void (*)(const SDL_Event&);
    static void SetGamepadEventHandler(EventHandler handler);
    // What F10 does: show or hide the frame rate counter (hle/video/fps_overlay.hpp).
    static void SetFpsKeyHandler(void (*handler)());

private:
    void OnResize();
    void SyncTextInput();

    SDL_Window* window = nullptr;
    std::atomic<s32> width{0};
    std::atomic<s32> height{0};
    WindowSystemInfo window_info{};
    bb::common::QuitChord quit_chord;
    bool text_input_active = false;  // only touched by the thread that pumps the events
};

}  // namespace Frontend
