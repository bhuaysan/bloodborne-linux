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
        self.assertTrue(description.startswith(b"bloodborne-linux rig pad\0"))
        # Not a real Xbox 360 pad's ids, so that real Xbox pads stay usable by programs that ignore
        # this one.
        self.assertEqual(struct.unpack_from("<HHHH", description, 80), (3, 0x1209, 0xB4B5, 0x0110))
        self.assertNotEqual((vpad.VENDOR_ID, vpad.PRODUCT_ID), (0x045E, 0x028E))

    def test_sdl_mapping_names_the_pad_by_its_guid(self) -> None:
        # SDL's Linux GUID: bus 3, crc 0, vendor, 0, product, 0, version, 0, little endian.
        self.assertEqual(vpad.sdl_guid(), "0300000009120000b5b4000010010000")
        mapping = vpad.sdl_mapping()
        self.assertTrue(mapping.startswith(vpad.sdl_guid() + ",bloodborne-linux rig pad,a:b0,"))
        self.assertTrue(mapping.endswith(",platform:Linux,"))
        for name in ("a", "b", "x", "y", "back", "guide", "start", "leftstick", "rightstick",
                     "leftshoulder", "rightshoulder", "dpup", "dpdown", "dpleft", "dpright",
                     "leftx", "lefty", "rightx", "righty", "lefttrigger", "righttrigger"):
            self.assertIn(f",{name}:", mapping)

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


class StreamPadTests(unittest.TestCase):
    def make(self) -> tuple[vpad.StreamPad, FakeDevice, list[float]]:
        stream, sleeps = FakeDevice(), []
        return vpad.StreamPad(stream, sleep=sleeps.append), stream, sleeps

    def lines(self, stream: FakeDevice) -> list[str]:
        return stream.getvalue().decode().splitlines()

    def test_tap_presses_waits_and_releases_as_uinput_does(self) -> None:
        pad, stream, sleeps = self.make()
        pad.tap("A", 50)
        self.assertEqual(self.lines(stream), ["button A 1", "button A 0"])
        self.assertEqual(sleeps, [0.05, 0.12])

    def test_dpad_and_trigger_use_the_uinput_axes(self) -> None:
        pad, stream, sleeps = self.make()
        pad.dpad("D")
        pad.trigger("R")
        self.assertEqual(self.lines(stream), ["axis HY 1", "axis HY 0", "axis RT 255", "axis RT 0"])
        self.assertEqual(sleeps, [0.12, 0.15, 0.2, 0.15])

    def test_stick_and_close(self) -> None:
        pad, stream, _ = self.make()
        pad.stick("LX", -32768)
        pad.close()
        self.assertEqual(self.lines(stream), ["axis LX -32768", "neutral"])

    def test_every_line_is_one_the_c_parser_knows(self) -> None:
        # The names of vpad.py are the ones src/common/pad_stream.cpp lists.
        known_buttons = {"A", "B", "X", "Y", "BACK", "GUIDE", "START", "L3", "R3", "LB", "RB"}
        known_axes = {"LX", "LY", "RX", "RY", "LT", "RT", "HX", "HY"}
        self.assertEqual(set(vpad.BUTTON_CODES), known_buttons)
        self.assertEqual(set(vpad.AXIS_CODES), known_axes)


if __name__ == "__main__":
    unittest.main()
