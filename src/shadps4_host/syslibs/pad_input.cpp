// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/sdl_window.cpp @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-11: SDLGamepadToOrbisButton, PollController, InitTimers
// and OnGamepadEvent as a handler of its own. The bindings of upstream's input config are
// written out as its defaults (buttons, sticks with the default deadzone, triggers); keyboard
// and mouse input are left out.
// License texts: third_party/shadps4/LICENSES/

#include "core/libraries/pad_input.h"

#include <algorithm>
#include <cstdlib>

#include <SDL3/SDL.h>

#include "common/singleton.h"
#include "core/libraries/pad/pad.h"
#include "input/controller.h"

namespace Libraries::Pad {

namespace {

Input::GameControllers& Controllers() {
    return *Common::Singleton<Input::GameControllers>::Instance();
}

OrbisPadButtonDataOffset SDLGamepadToOrbisButton(u8 button) {
    using OPBDO = OrbisPadButtonDataOffset;
    switch (button) {
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
        return OPBDO::Down;
    case SDL_GAMEPAD_BUTTON_DPAD_UP:
        return OPBDO::Up;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
        return OPBDO::Left;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
        return OPBDO::Right;
    case SDL_GAMEPAD_BUTTON_SOUTH:
        return OPBDO::Cross;
    case SDL_GAMEPAD_BUTTON_NORTH:
        return OPBDO::Triangle;
    case SDL_GAMEPAD_BUTTON_WEST:
        return OPBDO::Square;
    case SDL_GAMEPAD_BUTTON_EAST:
        return OPBDO::Circle;
    case SDL_GAMEPAD_BUTTON_START:
        return OPBDO::Options;
    case SDL_GAMEPAD_BUTTON_TOUCHPAD:
    case SDL_GAMEPAD_BUTTON_BACK:
        return OPBDO::TouchPad;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
        return OPBDO::L1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
        return OPBDO::R1;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK:
        return OPBDO::L3;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK:
        return OPBDO::R3;
    default:
        return OPBDO::None;
    }
}

Uint32 SDLCALL PollController(void* userdata, SDL_TimerID, Uint32 interval) {
    static_cast<Input::GameController*>(userdata)->PollState();
    return interval;
}

// Upstream's default deadzone, 1 to 127 on the axis scaled to -128..127: the part below the lower
// bound is zero, and the rest is stretched over the full range.
int ApplyDeadzone(int value) {
    constexpr int lower = 1;
    constexpr int upper = 127;
    if (std::abs(value) <= lower) {
        return 0;
    }
    return (value >= 0 ? 1 : -1) *
           std::clamp(static_cast<int>((128.0 * (std::abs(value) - lower)) / (upper - lower)), 0,
                      128);
}

// The pad slot of the gamepad an event came from, or null when no slot has it (a gamepad that
// came and went, or one no user plays on).
Input::GameController* ControllerOf(SDL_JoystickID id) {
    if (SDL_GetGamepadFromID(id) == nullptr) {
        return nullptr;
    }
    const u8 index = Controllers().GetGamepadIndexFromJoystickId(id);
    return index < 5 ? Controllers()[index] : nullptr;
}

void OnGamepadAxis(const SDL_GamepadAxisEvent& event) {
    Input::GameController* const controller = ControllerOf(event.which);
    if (controller == nullptr) {
        return;
    }
    // Upstream scales SDL's 16-bit axes to a byte first.
    const int value = std::clamp(static_cast<int>(event.value) / 256, -128, 127);
    switch (event.axis) {
    case SDL_GAMEPAD_AXIS_LEFTX:
    case SDL_GAMEPAD_AXIS_LEFTY:
    case SDL_GAMEPAD_AXIS_RIGHTX:
    case SDL_GAMEPAD_AXIS_RIGHTY: {
        const Input::Axis axis = event.axis == SDL_GAMEPAD_AXIS_LEFTX    ? Input::Axis::LeftX
                                 : event.axis == SDL_GAMEPAD_AXIS_LEFTY  ? Input::Axis::LeftY
                                 : event.axis == SDL_GAMEPAD_AXIS_RIGHTX ? Input::Axis::RightX
                                                                         : Input::Axis::RightY;
        controller->Axis(axis, Input::GetAxis(-0x80, 0x7f, ApplyDeadzone(value)), false);
        break;
    }
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: {
        const bool left = event.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER;
        const int pull = ApplyDeadzone(std::max(value, 0));
        controller->Axis(left ? Input::Axis::TriggerLeft : Input::Axis::TriggerRight,
                         Input::GetAxis(0x0, 0x7f, pull), false);
        controller->Button(left ? OrbisPadButtonDataOffset::L2 : OrbisPadButtonDataOffset::R2,
                           pull > 0x20);
        break;
    }
    default:
        break;
    }
}

}  // namespace

void HandleGamepadEvent(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
        Controllers().TryOpenSDLControllers();
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const OrbisPadButtonDataOffset button = SDLGamepadToOrbisButton(event.gbutton.button);
        if (button == OrbisPadButtonDataOffset::None) {
            break;
        }
        if (Input::GameController* c = ControllerOf(event.gbutton.which)) {
            c->Button(button, event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
        }
        break;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        OnGamepadAxis(event.gaxis);
        break;
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
        if (Input::GameController* c = ControllerOf(event.gtouchpad.which)) {
            c->SetTouchpadState(event.gtouchpad.finger, event.type != SDL_EVENT_GAMEPAD_TOUCHPAD_UP,
                                event.gtouchpad.x, event.gtouchpad.y);
        }
        break;
    case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
        if (Input::GameController* c = ControllerOf(event.gsensor.which)) {
            if (event.gsensor.sensor == SDL_SENSOR_GYRO) {
                c->UpdateGyro(event.gsensor.data);
            } else if (event.gsensor.sensor == SDL_SENSOR_ACCEL) {
                c->UpdateAcceleration(event.gsensor.data);
            }
        }
        break;
    default:
        break;
    }
}


void StartPolling() {
    // As upstream's InitTimers: each pad slot pushes its state every 4 ms, which is what moves
    // the smoothing of the sticks along between events.
    for (int i = 0; i < 4; ++i) {
        SDL_AddTimer(4, &PollController, Controllers()[i]);
    }
}

}  // namespace Libraries::Pad
