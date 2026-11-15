// SPDX-License-Identifier: GPL-2.0-or-later
// The host window's feed of SDL3 gamepad events into the imported pad (input/controller).
#pragma once

#include <string>

union SDL_Event;

namespace Libraries::Pad {

// Handles one SDL gamepad event as upstream's window does: a gamepad added or removed reopens the
// pad slots, buttons, sticks, triggers, the touchpad and the sensors update the slot's state.
void HandleGamepadEvent(const SDL_Event& event);

// Starts the 4 ms timers that make each pad slot push its state. Once, after the libraries
// registered.
void StartPolling();

// Attaches a virtual gamepad to SDL, inside this process only, and feeds it the pad states that
// the stream at `path` (a file or FIFO; bb::common::parse_pad_line has its format) carries from
// then on. SDL lists it like any gamepad, so the events take the path of a real pad's. It also
// makes SDL ignore every other controller (the game gives slot 0 to the first pad SDL lists).
// False, with the reason in `why`, when SDL or the stream refuses. Starts SDL's gamepad subsystem,
// and has to run before the pad library registers, which opens the pad slots.
bool OpenPadInput(const std::string& path, std::string& why);

}  // namespace Libraries::Pad
