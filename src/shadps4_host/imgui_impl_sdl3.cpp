// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/imgui/renderer/imgui_impl_sdl3.cpp @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-11: Init, Shutdown, the time step of NewFrame and OnResize
// only; no input, clipboard, IME, cursors, gamepads or frame rate statistics. Shutdown without a
// backend does nothing. 2026-11-22: ProcessEvent's keyboard, text, mouse and focus events (the
// editing keys and letters of KeyEventToImGuiKey), queued for NewFrame, since the events are pumped
// on another thread than the presenter's; ClearInput.
// License texts: third_party/shadps4/LICENSES/

// Based on imgui_impl_sdl3.cpp from Dear ImGui repository

#include <imgui.h>

#include <SDL3/SDL.h>

#include <cfloat>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "imgui_impl_sdl3.h"

namespace ImGui::Sdl {

namespace {

// One input event from ProcessEvent, waiting for NewFrame.
struct QueuedInput {
    enum class Kind { Clear, Key, Text, MousePos, MouseButton, MouseWheel, Focus };
    Kind kind{};
    ImGuiKey key = ImGuiKey_None;
    SDL_Keymod mods = 0;
    bool down = false;  // key or button pressed, window focused
    int button = 0;
    float x = 0.0f;
    float y = 0.0f;
    std::string text;  // SDL owns an event's text only until the next poll
};

// Bounds the queue if no frame drains it for a while; the oldest events go first.
constexpr std::size_t max_queued_inputs = 1024;
std::mutex input_mutex;
std::vector<QueuedInput> input_queue;

void Queue(QueuedInput input) {
    std::scoped_lock lock{input_mutex};
    if (input_queue.size() >= max_queued_inputs) {
        input_queue.erase(input_queue.begin());
    }
    input_queue.push_back(std::move(input));
}

// The keys text editing needs (letters for the Ctrl shortcuts); typed text comes as text events.
ImGuiKey KeyEventToImGuiKey(SDL_Keycode keycode, SDL_Scancode scancode) {
    if (scancode == SDL_SCANCODE_KP_ENTER) {
        return ImGuiKey_KeypadEnter;
    }
    if (keycode >= SDLK_A && keycode <= SDLK_Z) {
        return static_cast<ImGuiKey>(ImGuiKey_A + (keycode - SDLK_A));
    }
    switch (keycode) {
    case SDLK_TAB:
        return ImGuiKey_Tab;
    case SDLK_LEFT:
        return ImGuiKey_LeftArrow;
    case SDLK_RIGHT:
        return ImGuiKey_RightArrow;
    case SDLK_UP:
        return ImGuiKey_UpArrow;
    case SDLK_DOWN:
        return ImGuiKey_DownArrow;
    case SDLK_PAGEUP:
        return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN:
        return ImGuiKey_PageDown;
    case SDLK_HOME:
        return ImGuiKey_Home;
    case SDLK_END:
        return ImGuiKey_End;
    case SDLK_INSERT:
        return ImGuiKey_Insert;
    case SDLK_DELETE:
        return ImGuiKey_Delete;
    case SDLK_BACKSPACE:
        return ImGuiKey_Backspace;
    case SDLK_SPACE:
        return ImGuiKey_Space;
    case SDLK_RETURN:
        return ImGuiKey_Enter;
    case SDLK_ESCAPE:
        return ImGuiKey_Escape;
    case SDLK_LCTRL:
        return ImGuiKey_LeftCtrl;
    case SDLK_LSHIFT:
        return ImGuiKey_LeftShift;
    case SDLK_LALT:
        return ImGuiKey_LeftAlt;
    case SDLK_RCTRL:
        return ImGuiKey_RightCtrl;
    case SDLK_RSHIFT:
        return ImGuiKey_RightShift;
    case SDLK_RALT:
        return ImGuiKey_RightAlt;
    default:
        return ImGuiKey_None;
    }
}

void UpdateKeyModifiers(ImGuiIO& io, SDL_Keymod sdl_key_mods) {
    io.AddKeyEvent(ImGuiMod_Ctrl, (sdl_key_mods & SDL_KMOD_CTRL) != 0);
    io.AddKeyEvent(ImGuiMod_Shift, (sdl_key_mods & SDL_KMOD_SHIFT) != 0);
    io.AddKeyEvent(ImGuiMod_Alt, (sdl_key_mods & SDL_KMOD_ALT) != 0);
    io.AddKeyEvent(ImGuiMod_Super, (sdl_key_mods & SDL_KMOD_GUI) != 0);
}

// Hands the queued events to ImGui, on the presenter's thread before ImGui::NewFrame.
void ApplyQueuedInput(ImGuiIO& io) {
    std::vector<QueuedInput> inputs;
    {
        std::scoped_lock lock{input_mutex};
        inputs.swap(input_queue);
    }
    for (const QueuedInput& input : inputs) {
        switch (input.kind) {
        case QueuedInput::Kind::Clear:
            io.ClearEventsQueue();
            io.ClearInputKeys();
            io.ClearInputMouse();
            break;
        case QueuedInput::Kind::Key:
            UpdateKeyModifiers(io, input.mods);
            if (input.key != ImGuiKey_None) {
                io.AddKeyEvent(input.key, input.down);
            }
            break;
        case QueuedInput::Kind::Text:
            io.AddInputCharactersUTF8(input.text.c_str());
            break;
        case QueuedInput::Kind::MousePos:
            io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
            io.AddMousePosEvent(input.x, input.y);
            break;
        case QueuedInput::Kind::MouseButton:
            io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
            io.AddMouseButtonEvent(input.button, input.down);
            break;
        case QueuedInput::Kind::MouseWheel:
            io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
            io.AddMouseWheelEvent(input.x, input.y);
            break;
        case QueuedInput::Kind::Focus:
            io.AddFocusEvent(input.down);
            break;
        }
    }
}

}  // namespace

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
    {
        std::scoped_lock lock{input_mutex};
        input_queue.clear();
    }
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

    ApplyQueuedInput(io);
}

bool ProcessEvent(const SDL_Event* event) {
    using Kind = QueuedInput::Kind;
    QueuedInput input;
    switch (event->type) {
    case SDL_EVENT_MOUSE_MOTION:
        input.kind = Kind::MousePos;
        input.x = event->motion.x;
        input.y = event->motion.y;
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        input.kind = Kind::MouseWheel;
        input.x = -event->wheel.x;
        input.y = event->wheel.y;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        input.kind = Kind::MouseButton;
        input.down = event->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        switch (event->button.button) {
        case SDL_BUTTON_LEFT:
            input.button = ImGuiMouseButton_Left;
            break;
        case SDL_BUTTON_RIGHT:
            input.button = ImGuiMouseButton_Right;
            break;
        case SDL_BUTTON_MIDDLE:
            input.button = ImGuiMouseButton_Middle;
            break;
        default:
            return false;
        }
        break;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        input.kind = Kind::MousePos;
        input.x = -FLT_MAX;
        input.y = -FLT_MAX;
        break;
    case SDL_EVENT_TEXT_INPUT:
        if (event->text.text == nullptr) {
            return false;
        }
        input.kind = Kind::Text;
        input.text = event->text.text;
        break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        input.kind = Kind::Key;
        input.key = KeyEventToImGuiKey(event->key.key, event->key.scancode);
        input.mods = event->key.mod;
        input.down = event->type == SDL_EVENT_KEY_DOWN;
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        input.kind = Kind::Focus;
        input.down = event->type == SDL_EVENT_WINDOW_FOCUS_GAINED;
        break;
    default:
        return false;
    }
    Queue(std::move(input));
    return true;
}

void ClearInput() {
    std::scoped_lock lock{input_mutex};
    input_queue.clear();
    QueuedInput clear;
    clear.kind = QueuedInput::Kind::Clear;
    input_queue.push_back(std::move(clear));
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
