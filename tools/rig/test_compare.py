"""Tests of the menu comparison: PNG reading, the metric, the tolerance and the time to the menu."""

from __future__ import annotations

import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import compare  # noqa: E402
import report  # noqa: E402


def picture(value: int, width: int = 2, height: int = 2) -> compare.Picture:
    return compare.Picture(width, height, bytes([value]) * (width * height * 3))


def write_png(path: Path, width: int, height: int, rgba: bytes) -> None:
    """A PNG written the way the rig writes its shots, from an R8G8B8A8 capture."""
    raw = path.with_suffix(".raw")
    raw.write_bytes(f"BBRIGRAW1 {width} {height} 37\n".encode() + rgba)
    report.raw_to_png(raw, path)


def write_events(path: Path, rows: list[tuple[int, str, str]]) -> None:
    lines = ["mono_ns,utc,log_bytes,kind,text"]
    lines += [f"{ns},00:00:00.000,0,{kind},{text}" for ns, kind, text in rows]
    path.write_text("\n".join(lines) + "\n")


class PictureTests(unittest.TestCase):
    def test_a_written_shot_reads_back_as_rgb(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "a.png"
            write_png(path, 2, 1, bytes([1, 2, 3, 255, 4, 5, 6, 255]))
            self.assertEqual(compare.read_png(path), compare.Picture(2, 1, bytes([1, 2, 3, 4, 5, 6])))

    def test_identical_pictures_differ_by_zero(self) -> None:
        self.assertEqual(compare.difference(picture(77), picture(77)), 0.0)

    def test_one_channel_of_one_pixel(self) -> None:
        changed = bytearray(picture(0).rgb)
        changed[4] = 30  # green of the second pixel: 30 over 2 * 2 pixels * 3 channels
        self.assertEqual(compare.difference(picture(0), compare.Picture(2, 2, bytes(changed))), 2.5)

    def test_differences_are_absolute(self) -> None:
        self.assertEqual(compare.difference(picture(10), picture(4)), 6.0)
        self.assertEqual(compare.difference(picture(4), picture(10)), 6.0)

    def test_sizes_must_match(self) -> None:
        with self.assertRaisesRegex(ValueError, "different sizes"):
            compare.difference(picture(0, 2, 2), picture(0, 4, 1))

    def test_a_filtered_png_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "a.png"
            row = b"\x01" + bytes(3)  # one pixel with filter type Sub, which the rig never writes
            path.write_bytes(b"\x89PNG\r\n\x1a\n"
                             + report._chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0))
                             + report._chunk(b"IDAT", zlib.compress(row))
                             + report._chunk(b"IEND", b""))
            with self.assertRaisesRegex(ValueError, "filter"):
                compare.read_png(path)

    def test_mean_difference_takes_every_pair(self) -> None:
        # |10-12| and |10-20|, |14-12| and |14-20|: 2 + 10 + 2 + 6 over 4 pairs.
        self.assertEqual(compare.mean_difference([picture(10), picture(14)],
                                                 [picture(12), picture(20)]), 5.0)


class VerdictTests(unittest.TestCase):
    def check(self, ours: int, our_time: float = 20.0) -> dict:
        # The two reference runs differ by 2, so twice the floor allows a mean difference of 4.
        return compare.verdict([picture(10)], [picture(12)], [picture(ours)], [20.0, 30.0],
                               our_time)

    def test_ours_within_twice_the_floor_passes(self) -> None:
        result = self.check(13)  # 3 and 1 from the references
        self.assertEqual((result["noise_floor"], result["ours"], result["ratio"]), (2.0, 2.0, 1.0))
        self.assertTrue(result["pictures_ok"])
        self.assertTrue(result["ok"])

    def test_exactly_twice_the_floor_passes(self) -> None:
        result = self.check(15)  # 5 and 3
        self.assertEqual((result["ours"], result["ratio"]), (4.0, 2.0))
        self.assertTrue(result["ok"])

    def test_ours_over_twice_the_floor_fails(self) -> None:
        result = self.check(16)  # 6 and 4
        self.assertEqual((result["ours"], result["ratio"]), (5.0, 2.5))
        self.assertFalse(result["pictures_ok"])
        self.assertFalse(result["ok"])

    def test_a_zero_floor_allows_only_identical_pictures(self) -> None:
        same = compare.verdict([picture(10)], [picture(10)], [picture(10)], [20.0], 20.0)
        self.assertTrue(same["ok"])
        other = compare.verdict([picture(10)], [picture(10)], [picture(11)], [20.0], 20.0)
        self.assertEqual(other["ratio"], None)
        self.assertFalse(other["ok"])

    def test_time_is_measured_against_the_mean_reference_time(self) -> None:
        result = self.check(13, our_time=50.0)  # the references took 25 s on average
        self.assertEqual((result["time_ratio"], result["time_ok"]), (2.0, True))
        result = self.check(13, our_time=50.5)
        self.assertEqual((result["time_ratio"], result["time_ok"], result["ok"]), (2.02, False, False))


class TimeTests(unittest.TestCase):
    def test_time_runs_to_when_the_picture_stopped_changing(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "events.csv"
            write_events(path, [(1_000_000_000, "start", "bb-run"),
                                (2_000_000_000, "step", "12: stable seconds=6"),
                                (31_000_000_000, "stable", "unchanged for 6.250 s"),
                                (40_000_000_000, "stable", "unchanged for 1.000 s")])
            self.assertEqual(compare.time_to_stable(path), 23.75)

    def test_a_run_without_a_stable_wait_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "events.csv"
            write_events(path, [(1_000_000_000, "start", "bb-run")])
            with self.assertRaisesRegex(ValueError, "no start or no stable"):
                compare.time_to_stable(path)


class CommandTests(unittest.TestCase):
    def make_run(self, root: Path, name: str, value: int, menu_s: int) -> Path:
        run = root / name
        (run / "shots").mkdir(parents=True)
        write_png(run / "shots" / "menu-1.png", 2, 2, bytes([value, value, value, 255]) * 4)
        write_png(run / "shots" / "other.png", 2, 2, bytes(16))  # not a menu shot
        write_events(run / "events.csv", [(0, "start", name),
                                          (menu_s * 1_000_000_000, "stable",
                                           "unchanged for 0.000 s")])
        return run

    def run_rig(self, *runs: Path) -> subprocess.CompletedProcess:
        return subprocess.run([sys.executable, str(HERE / "rig.py"), "compare", *map(str, runs)],
                              capture_output=True, text=True)

    def test_exit_status_follows_the_verdict(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            a, b = self.make_run(root, "a", 10, 20), self.make_run(root, "b", 12, 30)
            passing = self.run_rig(a, b, self.make_run(root, "good", 13, 40))
            self.assertEqual(passing.returncode, 0, passing.stdout + passing.stderr)
            self.assertIn('"ratio": 1.0', passing.stdout)
            failing = self.run_rig(a, b, self.make_run(root, "bad", 16, 40))
            self.assertEqual(failing.returncode, 1, failing.stdout + failing.stderr)
            self.assertIn('"pictures_ok": false', failing.stdout)


if __name__ == "__main__":
    unittest.main()
