"""The rig's scripted gamepad, in two forms with the same taps and timing.

VirtualPad is an Xbox 360 style controller made through raw uinput (write access to /dev/uinput).
Every program on the desktop session sees it, so only the reference, a black box that reads
controllers from SDL, gets it. StreamPad writes the same states as text lines to bb-run's
--pad-input (src/common/pad_stream.hpp), which turns them into a gamepad inside its own process:
no device exists on the machine. Button and axis codes are those of Linux's input-event-codes.h.
"""

from __future__ import annotations

import fcntl
import os
import struct
import time
from collections.abc import Callable
from typing import BinaryIO

UI_SET_EVBIT, UI_SET_KEYBIT, UI_SET_ABSBIT = 0x40045564, 0x40045565, 0x40045567
UI_DEV_CREATE, UI_DEV_DESTROY = 0x5501, 0x5502
EV_SYN, EV_KEY, EV_ABS = 0, 1, 3
BUTTON_CODES = {"A": 0x130, "B": 0x131, "X": 0x133, "Y": 0x134, "LB": 0x136, "RB": 0x137,
                "BACK": 0x13A, "START": 0x13B, "GUIDE": 0x13C, "L3": 0x13D, "R3": 0x13E}
AXIS_CODES = {"LX": 0, "LY": 1, "LT": 2, "RX": 3, "RY": 4, "RT": 5, "HX": 0x10, "HY": 0x11}
AXIS_RANGES = {"LT": (0, 255), "RT": (0, 255), "HX": (-1, 1), "HY": (-1, 1)}
DEFAULT_RANGE = (-32768, 32767)
DPAD_AXES = {"U": ("HY", -1), "D": ("HY", 1), "L": ("HX", -1), "R": ("HX", 1)}
EVENT = struct.Struct("qqHHi")  # struct input_event on x86-64

# The uinput pad is not a real Xbox 360 pad's 045e:028e: programs that use SDL can ignore it with
# SDL_GAMECONTROLLER_IGNORE_DEVICES=0x1209/0xb4b5 without hiding real Xbox pads. The ids are
# arbitrary ones from the vendor id pid.codes shares among open hardware without an id of its own.
VENDOR_ID, PRODUCT_ID, VERSION = 0x1209, 0xB4B5, 0x0110
DEVICE_NAME = b"bloodborne-linux rig pad"
# SDL does not know the pad, so the reference needs its mapping (SDL_GAMECONTROLLERCONFIG). It is
# the one SDL has for Xbox 360 pads on Linux, because the buttons and axes are the same.
SDL_MAPPING_BODY = ("a:b0,b:b1,x:b2,y:b3,back:b6,guide:b8,start:b7,leftstick:b9,rightstick:b10,"
                    "leftshoulder:b4,rightshoulder:b5,dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,"
                    "leftx:a0,lefty:a1,rightx:a3,righty:a4,lefttrigger:a2,righttrigger:a5")


def sdl_guid() -> str:
    """SDL's GUID of the uinput pad on Linux: bus (USB), 0, vendor, 0, product, 0, version, 0."""
    return struct.pack("<8H", 3, 0, VENDOR_ID, 0, PRODUCT_ID, 0, VERSION, 0).hex()


def sdl_mapping() -> str:
    return f"{sdl_guid()},{DEVICE_NAME.decode()},{SDL_MAPPING_BODY},platform:Linux,"


def device_description() -> bytes:
    """struct uinput_user_dev for the pad: name, ids, then the four 64-entry axis arrays."""
    maximum, minimum = [0] * 64, [0] * 64
    for axis, code in AXIS_CODES.items():
        minimum[code], maximum[code] = AXIS_RANGES.get(axis, DEFAULT_RANGE)
    return (struct.pack("80sHHHHI", DEVICE_NAME, 3, VENDOR_ID, PRODUCT_ID, VERSION, 0)
            + struct.pack("64i", *maximum) + struct.pack("64i", *minimum)
            + struct.pack("64i", *[0] * 64) + struct.pack("64i", *[0] * 64))


class Pad:
    """The scripted inputs, on top of _key, _axis, neutral and close of a subclass."""

    _sleep: Callable[[float], None]

    def _key(self, name: str, down: bool) -> None:
        raise NotImplementedError

    def _axis(self, name: str, value: int) -> None:
        raise NotImplementedError

    def neutral(self) -> None:
        raise NotImplementedError

    def close(self) -> None:
        raise NotImplementedError

    def tap(self, name: str, ms: int = 120) -> None:
        self._key(name, True)
        self._sleep(ms / 1000)
        self._key(name, False)
        self._sleep(0.12)

    def dpad(self, direction: str, ms: int = 120) -> None:
        axis, value = DPAD_AXES[direction]
        self._axis(axis, value)
        self._sleep(ms / 1000)
        self._axis(axis, 0)
        self._sleep(0.15)

    def trigger(self, side: str, ms: int = 200) -> None:
        axis = "LT" if side == "L" else "RT"
        self._axis(axis, 255)
        self._sleep(ms / 1000)
        self._axis(axis, 0)
        self._sleep(0.15)

    def stick(self, axis: str, value: int) -> None:
        self._axis(axis, value)


class VirtualPad(Pad):
    def __init__(self, device: BinaryIO | None = None,
                 ioctl: Callable[[int, int, int], object] = fcntl.ioctl,
                 sleep: Callable[[float], None] = time.sleep):
        self._device = device if device is not None else open("/dev/uinput", "wb", buffering=0)
        self._fd = self._device.fileno() if device is None else -1
        self._ioctl = ioctl
        self._sleep = sleep
        self._control(UI_SET_EVBIT, EV_KEY)
        self._control(UI_SET_EVBIT, EV_ABS)
        for code in BUTTON_CODES.values():
            self._control(UI_SET_KEYBIT, code)
        for code in AXIS_CODES.values():
            self._control(UI_SET_ABSBIT, code)
        self._device.write(device_description())
        self._control(UI_DEV_CREATE, 0)
        self._sleep(1)  # udev needs a moment before a program starts looking for the device

    def _control(self, request: int, value: int) -> None:
        self._ioctl(self._fd, request, value)

    def _event(self, event_type: int, code: int, value: int) -> None:
        self._device.write(EVENT.pack(0, 0, event_type, code, value))

    def _sync(self) -> None:
        self._event(EV_SYN, 0, 0)

    def _key(self, name: str, down: bool) -> None:
        self._event(EV_KEY, BUTTON_CODES[name], int(down))
        self._sync()

    def _axis(self, name: str, value: int) -> None:
        self._event(EV_ABS, AXIS_CODES[name], value)
        self._sync()

    def neutral(self) -> None:
        for code in BUTTON_CODES.values():
            self._event(EV_KEY, code, 0)
        for code in AXIS_CODES.values():
            self._event(EV_ABS, code, 0)
        self._sync()

    def close(self) -> None:
        self.neutral()
        self._control(UI_DEV_DESTROY, 0)
        self._device.close()


class StreamPad(Pad):
    """The pad as text lines on a stream, for bb-run's --pad-input. Nothing is created on the
    machine, so no other program sees the input."""

    def __init__(self, stream: BinaryIO, sleep: Callable[[float], None] = time.sleep):
        self._stream = stream
        self._sleep = sleep

    def _line(self, text: str) -> None:
        self._stream.write(text.encode() + b"\n")
        self._stream.flush()

    def _key(self, name: str, down: bool) -> None:
        self._line(f"button {name} {int(down)}")

    def _axis(self, name: str, value: int) -> None:
        self._line(f"axis {name} {value}")

    def neutral(self) -> None:
        self._line("neutral")

    def close(self) -> None:
        self.neutral()
        self._stream.close()
