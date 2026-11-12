#!/usr/bin/env python3
"""Tests for the sound level reader. Run: python3 -m unittest discover -s tools/rig"""

from __future__ import annotations

import array
import math
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import audio  # noqa: E402


def write(directory: str, seconds: int, level: float, name: str = "a.raw") -> Path:
    path = Path(directory) / name
    samples = array.array("f", [level, -level] * (audio.RATE * audio.CHANNELS * seconds // 2))
    path.write_bytes(samples.tobytes())
    return path


class AudioTests(unittest.TestCase):
    def test_constant_level(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            result = audio.summary(write(d, 25, 0.25))
        self.assertEqual(result["windows"], 3)
        self.assertAlmostEqual(result["rms"], 0.25, places=5)
        self.assertAlmostEqual(result["peak"], 0.25, places=5)
        self.assertEqual(result["silent_windows"], 0)

    def test_silence_is_counted(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            result = audio.summary(write(d, 20, 0.0))
        self.assertEqual(result["silent_windows"], 2)
        self.assertEqual(result["rms"], 0.0)

    def test_span_limits_the_read(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            path = write(d, 30, 0.1)
            result = audio.summary(path, seconds=12)
        self.assertEqual(result["windows"], 2)

    def test_short_last_window_is_weighted_by_length(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            path = write(d, 10, 0.1)
            with open(path, "ab") as f:
                f.write(array.array("f", [0.4, -0.4] * (audio.RATE * audio.CHANNELS // 2)).tobytes())
            result = audio.summary(path)
        self.assertEqual(result["windows"], 2)
        self.assertAlmostEqual(result["rms"], math.sqrt((0.01 * 10 + 0.16) / 11), places=5)

    def test_span_not_a_multiple_of_the_window(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            path = write(d, 12, 0.1)
            result = audio.summary(path, seconds=10.5)
        self.assertEqual(result["windows"], 2)

    def test_partial_trailing_frame_is_ignored(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            path = write(d, 1, 0.2)
            with open(path, "ab") as f:
                f.write(b"\x01\x02\x03")
            result = audio.summary(path)
        self.assertAlmostEqual(result["rms"], 0.2, places=5)

    def test_empty_file(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "e.raw"
            path.write_bytes(b"")
            self.assertEqual(audio.summary(path)["windows"], 0)


if __name__ == "__main__":
    unittest.main()
