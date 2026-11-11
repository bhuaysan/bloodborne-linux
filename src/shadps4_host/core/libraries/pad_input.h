// SPDX-License-Identifier: GPL-2.0-or-later
// The host window's feed of SDL3 gamepad events into the imported pad (input/controller).
#pragma once

union SDL_Event;

namespace Libraries::Pad {

// Handles one SDL gamepad event as upstream's window does: a gamepad added or removed reopens the
// pad slots, buttons, sticks, triggers, the touchpad and the sensors update the slot's state.
void HandleGamepadEvent(const SDL_Event& event);

// Starts the 4 ms timers that make each pad slot push its state. Once, after the libraries
// registered.
void StartPolling();

}  // namespace Libraries::Pad
