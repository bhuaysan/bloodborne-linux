// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/sdl_window.cpp @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-11: the window creation and the native handles of the
// WindowSDL constructor and OnResize; X11 and Wayland only. Open and PumpEvents are ours: no
// input (the gamepad events go to a handler, syslibs/pad_input.cpp), camera, audio, fullscreen
// switching or hotkeys. 2026-11-21: Open takes the display to open on (FindDisplay). 2026-11-22:
// PumpEvents ends the run when Back and Start are held together (common/quit_chord.hpp, #757),
// and hands keyboard, text and mouse events to ImGui while the IME dialog is open (#795).
// 2026-11-24: F10 shows or hides the frame rate counter.
// License texts: third_party/shadps4/LICENSES/

#include "sdl_window.h"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
#include <cctype>

#include "imgui/renderer/imgui_core.h"
#include "imgui_impl_sdl3.h"

namespace Frontend {

namespace {
WindowSDL::EventHandler gamepad_handler = nullptr;
void (*fps_key_handler)() = nullptr;

bool ContainsIgnoringCase(std::string_view text, std::string_view part) {
    const auto lower = [](char c) { return std::tolower(static_cast<unsigned char>(c)); };
    return std::search(text.begin(), text.end(), part.begin(), part.end(),
                       [&](char a, char b) { return lower(a) == lower(b); }) != text.end();
}

// The display whose name contains `name`, ignoring case; 0 (the primary display) when `name` is
// empty or matches none, `note` then naming the displays there are.
SDL_DisplayID FindDisplay(std::string_view name, std::string& note) {
    note.clear();
    if (name.empty()) {
        return 0;
    }
    int count = 0;
    SDL_DisplayID* const ids = SDL_GetDisplays(&count);
    SDL_DisplayID found = 0;
    std::string names;
    // The whole name first (the launcher writes it), then the first name that contains it.
    for (int i = 0; i < count; ++i) {
        const char* const display = SDL_GetDisplayName(ids[i]);
        const std::string_view n = display != nullptr ? display : "";
        if (n == name) {
            found = ids[i];
            break;
        }
        if (found == 0 && ContainsIgnoringCase(n, name)) {
            found = ids[i];
        }
        names += names.empty() ? "" : ", ";
        names += n;
    }
    SDL_free(ids);
    if (found == 0) {
        note = "no display name contains \"" + std::string(name) + "\" (displays: " + names +
               "), using the primary display";
    }
    return found;
}
}  // namespace

void WindowSDL::SetGamepadEventHandler(EventHandler handler) {
    gamepad_handler = handler;
}

void WindowSDL::SetFpsKeyHandler(void (*handler)()) {
    fps_key_handler = handler;
}

std::unique_ptr<WindowSDL> WindowSDL::Open(std::string_view title, s32 width_, s32 height_,
                                           bool fullscreen, std::string_view display,
                                           std::string& display_note, std::string& why) {
    // SDL would otherwise turn SIGINT and SIGTERM into SDL_EVENT_QUIT, so Ctrl-C would not end the
    // run.
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    // The pad keeps its state while the window has no focus: the rig's virtual gamepad is never
    // the focused window's (the reference gets the same through its environment).
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        why = SDL_GetError();
        return nullptr;
    }
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, std::string(title).c_str());
    // A position on the display (not an undefined one) also makes SDL's Wayland backend ask for
    // fullscreen on that display's output.
    const SDL_DisplayID on = FindDisplay(display, display_note);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER,
                          SDL_WINDOWPOS_CENTERED_DISPLAY(on));
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER,
                          SDL_WINDOWPOS_CENTERED_DISPLAY(on));
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width_);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height_);
    SDL_SetNumberProperty(props, "flags", SDL_WINDOW_VULKAN);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    // With no fullscreen mode set, SDL's fullscreen is the borderless desktop kind.
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN, fullscreen);
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

std::string WindowSDL::DisplayName() const {
    const char* const name =
        window != nullptr ? SDL_GetDisplayName(SDL_GetDisplayForWindow(window)) : nullptr;
    return name != nullptr ? name : "";
}

WindowSDL::~WindowSDL() {
    if (window != nullptr) {
        SDL_DestroyWindow(window);
        SDL_Quit();
    }
}

// The keyboard and mouse belong to ImGui only while the IME dialog holds the input capture: the
// game reads neither, and events from before the dialog must not type into it. Text input runs
// only then too, without SDL's on-screen keyboard: the dialog draws its own for the pad.
void WindowSDL::SyncTextInput() {
    const bool wanted = ImGui::Core::IsGamepadInputCaptured();
    if (wanted == text_input_active) {
        return;
    }
    text_input_active = wanted;
    ImGui::Sdl::ClearInput();  // nothing held carries into the dialog or out of it
    if (wanted) {
        SDL_SetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD, "0");
        SDL_StartTextInput(window);
    } else {
        SDL_StopTextInput(window);
    }
}

bool WindowSDL::PumpEvents(s32 timeout_ms) {
    SDL_Event event;
    SyncTextInput();
    for (bool got = SDL_WaitEventTimeout(&event, timeout_ms); got; got = SDL_PollEvent(&event)) {
        SyncTextInput();
        if (text_input_active) {
            ImGui::Sdl::ProcessEvent(&event);
        }
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
        case SDL_EVENT_KEY_DOWN:
            // Not while the IME dialog takes the keyboard.
            if (event.key.key == SDLK_F10 && !event.key.repeat && !text_input_active &&
                fps_key_handler != nullptr) {
                fps_key_handler();
            }
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP: {
            const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
            const auto now = bb::common::QuitChord::Clock::now();
            if (event.gbutton.button == SDL_GAMEPAD_BUTTON_BACK) {
                quit_chord.back(down, now);
            } else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_START) {
                quit_chord.start(down, now);
            }
            if (gamepad_handler != nullptr) {
                gamepad_handler(event);
            }
            break;
        }
        case SDL_EVENT_GAMEPAD_REMOVED:
            quit_chord.reset();
            [[fallthrough]];
        case SDL_EVENT_GAMEPAD_ADDED:
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
        case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
        case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
        case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
            if (gamepad_handler != nullptr) {
                gamepad_handler(event);
            }
            break;
        default:
            break;
        }
    }
    return !quit_chord.held(bb::common::QuitChord::Clock::now());
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
