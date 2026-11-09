"""A virtual Xbox 360 style gamepad through raw uinput. It needs write access to /dev/uinput.

The target sees an ordinary controller, so the reference and, once it has a pad, our program get
the same input. Button and axis codes are those of Linux's input-event-codes.h.
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


def device_description() -> bytes:
    """struct uinput_user_dev for the pad: name, ids, then the four 64-entry axis arrays."""
    maximum, minimum = [0] * 64, [0] * 64
    for axis, code in AXIS_CODES.items():
        minimum[code], maximum[code] = AXIS_RANGES.get(axis, DEFAULT_RANGE)
    return (struct.pack("80sHHHHI", b"Microsoft X-Box 360 pad", 3, 0x045E, 0x028E, 0x0110, 0)
            + struct.pack("64i", *maximum) + struct.pack("64i", *minimum)
            + struct.pack("64i", *[0] * 64) + struct.pack("64i", *[0] * 64))


class VirtualPad:
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
