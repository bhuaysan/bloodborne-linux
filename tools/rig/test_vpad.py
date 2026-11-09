#!/usr/bin/env python3
"""Tests for the virtual gamepad. Run: python3 -m unittest discover -s tools/rig

No test here starts the game or needs a GPU, /dev/uinput or systemd.
"""

from __future__ import annotations

import io
import struct
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import vpad  # noqa: E402


class FakeDevice(io.BytesIO):
    def close(self) -> None:  # keep contents readable after the pad is closed
        pass


def fake_pad() -> tuple[vpad.VirtualPad, FakeDevice, list[tuple[int, int]]]:
    device = FakeDevice()
    calls: list[tuple[int, int]] = []
    pad = vpad.VirtualPad(device, ioctl=lambda fd, request, value: calls.append((request, value)),
                          sleep=lambda s: None)
    return pad, device, calls


def events_written(device: FakeDevice, offset: int) -> list[tuple[int, int, int]]:
    data = device.getvalue()[offset:]
    return [struct.unpack("qqHHi", data[i:i + 24])[2:] for i in range(0, len(data), 24)]


class VirtualPadTests(unittest.TestCase):
    def test_device_description_size_and_ids(self) -> None:
        description = vpad.device_description()
        self.assertEqual(len(description), 80 + 4 * 2 + 4 + 4 * 64 * 4)
        self.assertTrue(description.startswith(b"Microsoft X-Box 360 pad\0"))
        self.assertEqual(struct.unpack_from("<HHHH", description, 80), (3, 0x045E, 0x028E, 0x0110))

    def test_trigger_axes_have_their_own_range(self) -> None:
        description = vpad.device_description()
        maximum = struct.unpack_from("<64i", description, 92)
        minimum = struct.unpack_from("<64i", description, 92 + 64 * 4)
        self.assertEqual((minimum[vpad.AXIS_CODES["RT"]], maximum[vpad.AXIS_CODES["RT"]]), (0, 255))
        self.assertEqual((minimum[vpad.AXIS_CODES["HX"]], maximum[vpad.AXIS_CODES["HX"]]), (-1, 1))
        self.assertEqual(maximum[vpad.AXIS_CODES["LX"]], 32767)

    def test_setup_creates_the_device_last(self) -> None:
        _, _, calls = fake_pad()
        self.assertEqual(calls[-1], (vpad.UI_DEV_CREATE, 0))
        self.assertIn((vpad.UI_SET_KEYBIT, vpad.BUTTON_CODES["A"]), calls)
        self.assertIn((vpad.UI_SET_ABSBIT, vpad.AXIS_CODES["RT"]), calls)

    def test_tap_presses_then_releases(self) -> None:
        pad, device, _ = fake_pad()
        offset = len(device.getvalue())
        pad.tap("A", 50)
        self.assertEqual(events_written(device, offset),
                         [(vpad.EV_KEY, 0x130, 1), (vpad.EV_SYN, 0, 0),
                          (vpad.EV_KEY, 0x130, 0), (vpad.EV_SYN, 0, 0)])

    def test_dpad_down_uses_the_hat_axis(self) -> None:
        pad, device, _ = fake_pad()
        offset = len(device.getvalue())
        pad.dpad("D")
        self.assertEqual(events_written(device, offset),
                         [(vpad.EV_ABS, 0x11, 1), (vpad.EV_SYN, 0, 0),
                          (vpad.EV_ABS, 0x11, 0), (vpad.EV_SYN, 0, 0)])

    def test_right_trigger_goes_to_full_and_back(self) -> None:
        pad, device, _ = fake_pad()
        offset = len(device.getvalue())
        pad.trigger("R")
        self.assertEqual(events_written(device, offset),
                         [(vpad.EV_ABS, 5, 255), (vpad.EV_SYN, 0, 0),
                          (vpad.EV_ABS, 5, 0), (vpad.EV_SYN, 0, 0)])

    def test_close_releases_and_destroys(self) -> None:
        pad, device, calls = fake_pad()
        pad.tap("B")
        pad.close()
        self.assertEqual(calls[-1], (vpad.UI_DEV_DESTROY, 0))
        self.assertIn((vpad.EV_KEY, vpad.BUTTON_CODES["B"], 0), events_written(device, len(vpad.device_description())))


if __name__ == "__main__":
    unittest.main()
