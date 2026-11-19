"""Tests of load_times.py: the HUD test, the Continue press, and the death and travel loads."""

from __future__ import annotations

import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import compare  # noqa: E402
import load_times  # noqa: E402
import report  # noqa: E402

WIDTH, HEIGHT = 320, 180  # a quarter of 1280x720: the stamina box scales with the picture
S = 1_000_000_000


def frame(hud: bool) -> compare.Picture:
    rgb = bytearray(b"\x20\x20\x20" * (WIDTH * HEIGHT))
    if hud:
        for y in range(14, 15):
            for x in range(18, 43):
                rgb[(y * WIDTH + x) * 3:(y * WIDTH + x) * 3 + 3] = b"\x30\xa0\x30"
    return compare.Picture(WIDTH, HEIGHT, bytes(rgb))


def write_shot(path: Path, hud: bool) -> None:
    picture = frame(hud)
    rgba = b"".join(picture.rgb[i:i + 3] + b"\xff" for i in range(0, len(picture.rgb), 3))
    raw = path.with_suffix(".raw")
    raw.write_bytes(f"BBRIGRAW1 {WIDTH} {HEIGHT} 37\n".encode() + rgba)
    report.raw_to_png(raw, path)


class Run:
    """A run directory built row by row: events, shots and the target's log."""

    def __init__(self, root: Path):
        self.dir = root
        (root / "shots").mkdir()
        self.rows: list[tuple[int, int, str, str]] = []
        self.log = b""
        self.shots = 0

    def log_line(self, text: str) -> None:
        self.log += text.encode() + b"\n"

    def event(self, seconds: float, kind: str, text: str) -> None:
        self.rows.append((int(seconds * S), len(self.log), kind, text))

    def shot(self, seconds: float, hud: bool) -> None:
        self.shots += 1
        name = f"s{self.shots:03d}"
        write_shot(self.dir / "shots" / f"{name}.png", hud)
        self.event(seconds, "step", f"20: shot {name}")

    def finish(self, finished: bool = True) -> Path:
        lines = ["mono_ns,utc,log_bytes,kind,text"]
        lines += [f"{ns},00:00:00.000,{size},{kind},{text}" for ns, size, kind, text in self.rows]
        if finished:
            lines.append(f"{self.rows[-1][0] + 1},00:00:00.000,{len(self.log)},end,scenario finished")
        (self.dir / "events.csv").write_text("\n".join(lines) + "\n")
        (self.dir / "target.log").write_bytes(self.log)
        (self.dir / "summary.json").write_text(json.dumps({"target": "bb-run", "scenario": "x"}))
        return self.dir


class HudTests(unittest.TestCase):
    def test_the_stamina_bar_is_found_at_any_scale(self) -> None:
        self.assertGreater(load_times.stamina(frame(True)), load_times.STAMINA_MIN)
        self.assertEqual(load_times.stamina(frame(False)), 0.0)


class LoadTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.run = Run(Path(self.tmp.name))

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_continue_starts_at_the_press_before_the_load_line(self) -> None:
        r = self.run
        r.log_line("hle.libScePlayGo: 4AAcTU9R3XM: mode 1: ignored")  # at boot, before the menu
        r.event(1, "mark", "menu")
        r.event(1, "step", "11: tap A")  # a notice
        r.shot(2, False)
        r.event(4, "step", "11: tap A")  # Continue
        r.log_line("hle.libScePlayGo: 4AAcTU9R3XM: mode 1: ignored")
        r.shot(5, False)
        r.event(7, "step", "11: tap A")  # during the load
        r.shot(10, False)
        r.shot(11, True)
        r.shot(12, True)
        (load,) = load_times.loads(r.finish())
        self.assertEqual(load.name, "continue")
        self.assertAlmostEqual(load.seconds, 6.5)  # from 4 to between 10 and 11
        self.assertAlmostEqual(load.uncertainty, 0.5)

    def test_the_reference_logs_the_call_by_name(self) -> None:
        r = self.run
        r.event(1, "mark", "menu")
        r.event(1, "step", "11: tap A")
        r.log_line("[Lib.PlayGo] <Info> (Game:Main) playgo.cpp:319 scePlayGoSetInstallSpeed: called")
        r.shot(2, False)
        r.shot(3, True)
        (load,) = load_times.loads(r.finish())
        self.assertAlmostEqual(load.seconds, 1.5)

    def test_death_runs_from_the_hud_going_to_it_coming_back(self) -> None:
        r = self.run
        r.event(0, "mark", "death")
        r.shot(1, True)
        r.shot(2, True)
        r.shot(3, False)  # YOU DIED: died between 2 and 3
        r.shot(15, False)
        r.shot(16, True)  # back between 15 and 16
        (load,) = load_times.loads(r.finish())
        self.assertEqual(load.name, "death")
        self.assertAlmostEqual(load.seconds, 13.0)
        self.assertAlmostEqual(load.uncertainty, 1.0)

    def test_travel_needs_the_hud_to_go_after_the_press(self) -> None:
        r = self.run
        r.event(0, "mark", "travel")
        r.event(0, "step", "40: tap A")
        r.shot(1, True)  # the press still shows the HUD for a moment
        r.shot(2, False)
        r.shot(9, False)
        r.shot(10, True)
        (load,) = load_times.loads(r.finish())
        self.assertEqual(load.name, "travel")
        self.assertAlmostEqual(load.seconds, 9.5)

    def test_travel_whose_hud_is_gone_by_the_first_picture(self) -> None:
        r = self.run
        r.shot(0, False)  # the question, which hides the HUD
        r.event(0.5, "mark", "travel")
        r.event(0.5, "step", "40: tap A")
        r.shot(1, False)
        r.shot(8, False)
        r.shot(9, True)
        (load,) = load_times.loads(r.finish())
        self.assertAlmostEqual(load.seconds, 8.0)

    def test_continue_without_the_load_line_is_left_out(self) -> None:
        r = self.run
        r.event(1, "mark", "menu")
        r.event(1, "step", "11: tap A")
        r.shot(2, False)
        r.shot(3, True)
        self.assertEqual(load_times.loads(r.finish()), [])

    def test_a_load_that_never_ends_is_left_out(self) -> None:
        r = self.run
        r.event(0, "mark", "travel")
        r.event(0, "step", "40: tap A")
        r.shot(1, False)
        r.shot(2, False)
        self.assertEqual(load_times.loads(r.finish()), [])

    def test_an_unfinished_run_is_listed_but_not_summed(self) -> None:
        r = self.run
        r.event(0, "mark", "travel")
        r.event(0, "step", "40: tap A")
        r.shot(1, False)
        r.shot(2, True)
        out = io.StringIO()
        with redirect_stdout(out):
            load_times.main([str(r.finish(finished=False))])
        self.assertIn("(scenario not finished)", out.getvalue())
        self.assertNotIn("median", out.getvalue())

    def test_runs_are_summed_per_load_and_a_broken_run_is_skipped(self) -> None:
        with tempfile.TemporaryDirectory() as other:
            runs = []
            for root, end in ((self.run.dir, 2), (Path(other), 4)):
                r = self.run if root == self.run.dir else Run(root)
                r.event(0, "mark", "travel")
                r.event(0, "step", "40: tap A")
                r.shot(1, False)
                r.shot(end, True)
                runs.append(str(r.finish()))
            broken = Path(other) / "broken"
            broken.mkdir()
            out = io.StringIO()
            with redirect_stdout(out):
                load_times.main([*runs, str(broken)])
        self.assertIn("broken: unreadable run", out.getvalue())
        self.assertIn("travel   median   2.0 s, spread  1.0 s, runs 2: 1.5 2.5", out.getvalue())

    def test_a_run_the_rig_marked_failed_is_not_summed(self) -> None:
        r = self.run
        r.event(0, "mark", "travel")
        r.event(0, "step", "40: tap A")
        r.shot(1, False)
        r.shot(2, True)
        r.finish()
        (r.dir / "summary.json").write_text(json.dumps(
            {"target": "bb-run", "scenario": "x", "scenario_ok": False}))
        out = io.StringIO()
        with redirect_stdout(out):
            load_times.main([str(r.dir)])
        self.assertNotIn("median", out.getvalue())


if __name__ == "__main__":
    unittest.main()
