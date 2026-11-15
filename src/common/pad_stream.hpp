// The text stream of pad states that bb-run reads with --pad-input (docs/test-rig.md): the rig's
// scenario input without a system-wide /dev/uinput device. Parsing is separate from SDL so that it
// is unit-tested without a GPU or a window.
//
// One command per line, names and ranges as tools/rig/vpad.py has them:
//   button NAME 0|1     A B X Y LB RB BACK START GUIDE L3 R3
//   axis NAME VALUE     LX LY RX RY: -32768..32767, LT RT: 0..255, HX HY (the d-pad): -1..1
//   neutral             every button up, sticks and d-pad centered, triggers released
// Blank lines and lines starting with # are skipped.
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace bb::common {

// The ids of the virtual joystick bb-run attaches inside its own process. SDL_GAMECONTROLLER_IGNORE
// _DEVICES_EXCEPT names them, so that real controllers don't take pad slot 0 (#520).
inline constexpr std::uint16_t pad_stream_vendor = 0x1209;
inline constexpr std::uint16_t pad_stream_product = 0xB4B4;

// The virtual joystick's layout is SDL's default for a gamepad-type virtual joystick: buttons
// a b x y back guide start leftstick rightstick leftshoulder rightshoulder dpup dpdown dpleft
// dpright, axes leftx lefty rightx righty lefttrigger righttrigger.
inline constexpr int pad_stream_buttons = 15;
inline constexpr int pad_stream_axes = 6;

// A change of one button (value 0 or 1) or one axis (SDL's -32768..32767) of the virtual joystick.
struct PadUpdate {
    bool is_axis;
    int index;
    int value;

    friend bool operator==(const PadUpdate&, const PadUpdate&) = default;
};

// The updates one line stands for: none for a blank line or a comment, two for a d-pad axis (it
// presses one button and releases the opposite). Throws std::invalid_argument for anything else.
std::vector<PadUpdate> parse_pad_line(std::string_view line);

// The updates of `neutral`, which are also the state the joystick starts in. A trigger at rest is
// the axis minimum, not 0: SDL scales -32768..32767 to the trigger's 0..32767.
std::vector<PadUpdate> neutral_pad_updates();

}  // namespace bb::common
