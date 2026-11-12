#!/usr/bin/env python3
"""Tests for the run report: frame times, pictures, kernel faults, game directory. Run: python3 -m unittest discover -s tools/rig

No test here starts the game or needs a GPU, /dev/uinput or systemd.
"""

from __future__ import annotations

import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import report  # noqa: E402


class ReportTests(unittest.TestCase):
    def frames(self, gaps: list[float], captured: list[int] | None = None) -> list[report.Frame]:
        result, now = [], 1000
        for i, gap in enumerate([0, *gaps]):
            now += int(gap * 1e6)
            result.append(report.Frame(now, bool(captured and i in captured)))
        return result

    def test_stats_of_even_frames(self) -> None:
        stats = report.frame_stats(self.frames([33.0] * 100))
        self.assertEqual(stats["frames"], 101)
        self.assertEqual(stats["fps"], 30.3)
        self.assertEqual(stats["p50_ms"], 33.0)
        self.assertEqual(stats["stutters_over_2x_median"], 0)

    def test_p99_is_the_nearest_rank_not_the_maximum(self) -> None:
        stats = report.frame_stats(self.frames([1.0] * 99 + [50.0]))
        self.assertEqual(stats["p99_ms"], 1.0)
        self.assertEqual(stats["max_ms"], 50.0)

    def test_stutter_is_counted(self) -> None:
        stats = report.frame_stats(self.frames([33.0] * 50 + [100.0] + [33.0] * 49))
        self.assertEqual(stats["max_ms"], 100.0)
        self.assertEqual(stats["stutters_over_2x_median"], 1)

    def test_intervals_around_a_capture_are_dropped(self) -> None:
        # Frame 3 is captured, so the intervals into and out of it leave the statistics.
        frames = self.frames([10.0, 10.0, 500.0, 500.0, 10.0], captured=[3])
        self.assertEqual(report.intervals_ms(frames), [10.0, 10.0, 10.0])

    def test_no_frames_gives_no_stats(self) -> None:
        self.assertIsNone(report.frame_stats([]))
        self.assertIsNone(report.frame_stats(self.frames([])))

    def test_segments_split_at_marks(self) -> None:
        frames = [report.Frame(ns, False) for ns in (0, 10, 20, 100, 110, 120)]
        segments = report.segment_stats(frames, [(0, "a"), (50, "b")])
        self.assertEqual(sorted(segments), ["a", "b"])
        self.assertEqual(segments["a"]["frames"], 3)
        self.assertEqual(segments["b"]["frames"], 3)

    def test_a_segment_without_presents_is_recorded_as_none(self) -> None:
        frames = self.frames([10, 10, 10])
        segments = report.segment_stats(frames, [(0, "a"), (10**9, "empty")])
        self.assertEqual(sorted(segments), ["a", "empty"])
        self.assertIsNone(segments["empty"])

    def test_read_frames_skips_a_cut_off_last_line(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "frames.csv"
            path.write_text("frame,present_ns,return_ns,result,width,height,captured\n"
                            "1,100,101,0,1280,720,1\n2,200,201,0,1280,720,0\n3,30")
            frames = report.read_frames(path)
        self.assertEqual([f.present_ns for f in frames], [100, 200])
        self.assertTrue(frames[0].captured)

    def test_kernel_faults_are_found(self) -> None:
        journal = ("kernel: amdgpu 0000:03:00.0: [gfxhub] page fault\n"
                   "kernel: usb 1-1: new device\n"
                   "kernel: [drm:amdgpu_job_timedout] ring gfx_0.0.0 timeout\n")
        self.assertEqual(len(report.kernel_faults(journal)), 2)
        self.assertEqual(report.kernel_faults("kernel: usb 1-1: new device\n"), [])

    def test_tree_changes(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "dir").mkdir()
            (root / "dir" / "a").write_text("x")
            (root / "b").write_text("y")
            before = report.tree_state(root)
            self.assertEqual(report.tree_changes(before, report.tree_state(root)), [])
            (root / "b").write_text("longer")
            (root / "dir" / "a").unlink()
            (root / "c").write_text("z")
            changes = report.tree_changes(before, report.tree_state(root))
            for expected in ("added: c", "changed: b", "removed: dir/a"):
                self.assertIn(expected, changes)

    def test_tree_state_refuses_a_missing_root(self) -> None:
        with self.assertRaises(NotADirectoryError):
            report.tree_state(Path("/no/such/directory"))

    def png_pixels(self, path: Path) -> tuple[int, int, bytes]:
        data = path.read_bytes()
        self.assertEqual(data[:8], b"\x89PNG\r\n\x1a\n")
        width, height = struct.unpack(">II", data[16:24])
        length = struct.unpack(">I", data[33:37])[0]
        rows = zlib.decompress(data[41:41 + length])
        stride = width * 3 + 1
        self.assertEqual(len(rows), height * stride)
        return width, height, b"".join(rows[y * stride + 1:(y + 1) * stride] for y in range(height))

    def convert(self, width: int, height: int, vk_format: int, pixels: bytes) -> tuple[int, int, bytes]:
        with tempfile.TemporaryDirectory() as temp:
            raw, png = Path(temp) / "a.raw", Path(temp) / "a.png"
            raw.write_bytes(f"BBRIGRAW1 {width} {height} {vk_format}\n".encode() + pixels)
            report.raw_to_png(raw, png)
            return self.png_pixels(png)

    def test_png_from_bgra(self) -> None:
        pixels = bytes([1, 2, 3, 255, 4, 5, 6, 255])
        self.assertEqual(self.convert(2, 1, 50, pixels), (2, 1, bytes([3, 2, 1, 6, 5, 4])))

    def test_png_from_rgba(self) -> None:
        pixels = bytes([1, 2, 3, 255])
        self.assertEqual(self.convert(1, 1, 37, pixels)[2], bytes([1, 2, 3]))

    def test_png_from_a2b10g10r10(self) -> None:
        value = (0x3FF << 0) | (0x200 << 10) | (0x004 << 20)  # r full, g half, b tiny
        pixels = struct.pack("<I", value)
        self.assertEqual(self.convert(1, 1, 64, pixels)[2], bytes([0xFF, 0x80, 0x01]))

    def test_png_from_srgb_rgba_and_a2r10g10b10(self) -> None:
        self.assertEqual(self.convert(1, 1, 43, bytes([9, 8, 7, 255]))[2], bytes([9, 8, 7]))
        value = (0x3FF << 20) | (0x200 << 10) | 0x004  # r full, g half, b tiny
        self.assertEqual(self.convert(1, 1, 58, struct.pack("<I", value))[2], bytes([0xFF, 0x80, 0x01]))

    def test_png_rejects_a_truncated_capture(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            raw = Path(temp) / "a.raw"
            raw.write_bytes(b"BBRIGRAW1 2 2 50\nxxxx")
            with self.assertRaisesRegex(ValueError, "expected 16 bytes"):
                report.raw_to_png(raw, Path(temp) / "a.png")

    def test_png_rejects_an_unknown_format(self) -> None:
        with self.assertRaisesRegex(ValueError, "unsupported"):
            self.convert(1, 1, 999, bytes(4))


if __name__ == "__main__":
    unittest.main()
