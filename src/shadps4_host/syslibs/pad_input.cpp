// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/sdl_window.cpp @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-11: SDLGamepadToOrbisButton, PollController, InitTimers
// and OnGamepadEvent as a handler of its own. The bindings of upstream's input config are
// written out as its defaults (buttons, sticks with the default deadzone, triggers); keyboard
// and mouse input are left out.
// License texts: third_party/shadps4/LICENSES/

#include "core/libraries/pad_input.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <format>
#include <stdexcept>
#include <thread>

#include <SDL3/SDL.h>

#include "common/log.hpp"
#include "common/pad_stream.hpp"
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


namespace {

void Apply(SDL_Joystick* joystick, const std::vector<bb::common::PadUpdate>& updates) {
    for (const bb::common::PadUpdate& u : updates) {
        if (u.is_axis) {
            SDL_SetJoystickVirtualAxis(joystick, u.index, static_cast<Sint16>(u.value));
        } else {
            SDL_SetJoystickVirtualButton(joystick, u.index, u.value != 0);
        }
    }
}

// Reads the stream until the process ends. Each line is applied as it arrives, so a tap lasts as
// long as the writer waits between its two lines, as it does on a uinput device.
void ReadPadStream(int fd, SDL_Joystick* joystick) {
    constexpr std::size_t max_line = 256;  // the longest valid line has about 20 characters
    std::string pending;
    bool dropping = false;  // inside a line that was too long, up to its newline
    char buffer[4096];
    for (;;) {
        const ssize_t got = read(fd, buffer, sizeof(buffer));
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            bb::common::log(bb::common::LogLevel::warn, "pad", "pad input ends, {}: no more input",
                            got == 0 ? "end of file" : std::strerror(errno));
            return;
        }
        pending.append(buffer, static_cast<std::size_t>(got));
        if (dropping) {
            const std::size_t nl = pending.find('\n');
            pending.erase(0, nl == std::string::npos ? pending.size() : nl + 1);
            dropping = nl == std::string::npos;
        }
        if (!dropping && pending.find('\n') == std::string::npos && pending.size() > max_line) {
            bb::common::log(bb::common::LogLevel::warn, "pad",
                            "pad input line longer than {} bytes dropped", max_line);
            pending.clear();
            dropping = true;
        }
        for (std::size_t end; (end = pending.find('\n')) != std::string::npos;
             pending.erase(0, end + 1)) {
            const std::string line = pending.substr(0, end);
            bb::common::log(bb::common::LogLevel::debug, "pad", "pad input \"{}\"", line);
            try {
                Apply(joystick, bb::common::parse_pad_line(line));
            } catch (const std::invalid_argument& e) {
                bb::common::log(bb::common::LogLevel::warn, "pad", "pad input \"{}\": {}", line,
                                e.what());
            }
        }
    }
}

}  // namespace

bool OpenPadInput(const std::string& path, std::string& why) {
    // Read-write, so the open neither waits for a writer on a FIFO nor sees an end of file when
    // the writer is not there yet.
    const int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        why = path + ": " + std::strerror(errno);
        return false;
    }
    // As InitHLELibs does, and before SDL starts: a later hint would not remove the handlers SDL
    // installs for SIGINT and SIGTERM, so `timeout` and the rig's stop would not end the run.
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    // Only the virtual pad counts. The environment's value of the hint, which the rig sets for
    // the reference, must not win.
    const std::string only = std::format("0x{:04x}/0x{:04x}", bb::common::pad_stream_vendor,
                                         bb::common::pad_stream_product);
    SDL_SetHintWithPriority(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, only.c_str(),
                            SDL_HINT_OVERRIDE);
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.vendor_id = bb::common::pad_stream_vendor;
    desc.product_id = bb::common::pad_stream_product;
    desc.naxes = bb::common::pad_stream_axes;
    desc.nbuttons = bb::common::pad_stream_buttons;
    desc.name = "bloodborne-linux input stream";
    const SDL_JoystickID id =
        SDL_InitSubSystem(SDL_INIT_GAMEPAD) ? SDL_AttachVirtualJoystick(&desc) : 0;
    SDL_Joystick* const joystick = id != 0 ? SDL_OpenJoystick(id) : nullptr;
    if (joystick == nullptr) {
        why = SDL_GetError();
        close(fd);
        return false;
    }
    Apply(joystick, bb::common::neutral_pad_updates());
    std::thread(ReadPadStream, fd, joystick).detach();
    return true;
}

void StartPolling() {
    // As upstream's InitTimers: each pad slot pushes its state every 4 ms, which is what moves
    // the smoothing of the sticks along between events.
    for (int i = 0; i < 4; ++i) {
        SDL_AddTimer(4, &PollController, Controllers()[i]);
    }
}

}  // namespace Libraries::Pad
