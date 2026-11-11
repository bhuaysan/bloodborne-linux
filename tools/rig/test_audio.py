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

    def test_empty_file(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "e.raw"
            path.write_bytes(b"")
            self.assertEqual(audio.summary(path)["windows"], 0)

    def test_ratio(self) -> None:
        self.assertAlmostEqual(audio.ratio({"rms": 0.02}, {"rms": 0.04}), 0.5)
        self.assertIsNone(audio.ratio({"rms": 0.02}, {"rms": 0.0}))
        self.assertTrue(math.isfinite(audio.ratio({"rms": 0.0}, {"rms": 1.0})))


if __name__ == "__main__":
    unittest.main()
