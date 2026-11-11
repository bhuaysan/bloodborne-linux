#!/usr/bin/env python3
"""Tests for scenario scripts and their interpreter. Run: python3 -m unittest discover -s tools/rig

No test here starts the game or needs a GPU, /dev/uinput or systemd.
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import scenario as scn  # noqa: E402


class ParseTests(unittest.TestCase):
    def test_steps_and_comments(self) -> None:
        parsed = scn.parse("# start\nlimit time=300 memory=8G\ntap A ms=50 count=2\n\nshot one  # x\n")
        self.assertEqual(parsed.time_limit, 300)
        self.assertEqual(parsed.memory_limit, "8G")
        self.assertEqual([s.op for s in parsed.steps], ["tap", "shot"])
        self.assertEqual(parsed.steps[0].count(), 2)
        self.assertEqual(parsed.steps[0].line, 3)

    def test_quoted_regex_keeps_spaces(self) -> None:
        parsed = scn.parse('log "name = [a-z]+" timeout=5\n')
        self.assertEqual(parsed.steps[0].args, ["name = [a-z]+"])
        self.assertEqual(parsed.steps[0].opts, {"timeout": "5"})

    def test_uses_pad_and_frames(self) -> None:
        self.assertTrue(scn.parse("dpad U\n").uses_pad())
        self.assertFalse(scn.parse("log x\n").uses_pad())
        self.assertTrue(scn.parse("stable\n").uses_frames())
        self.assertFalse(scn.parse("sleep 1\n").uses_frames())

    def test_errors_name_the_line(self) -> None:
        bad = {"tap Z": "unknown button", "dpad Q": "unknown direction", "trigger X": "unknown trigger",
               "stick LX 99999": "out of range", "stick QQ 1": "unknown axis",
               "fly": "unknown step", "tap A nope=1": "takes 1 argument", "tap": "takes 1 argument",
               "sleep -1": "negative sleep", "frames 0": "positive", "log (": "line 2",
               "shot a/b": "bad name", "mark a.b": "bad name", "tap A count=0": "count",
               "stable delta=-1": "negative delta", "limit nope=1": "limit takes",
               "sleep nan": "finite", "log x timeout=inf": "finite", "limit time=0": "positive",
               "limit memory=lots": "12G"}
        for text, expected in bad.items():
            with self.subTest(text), self.assertRaises(scn.ScenarioError) as caught:
                scn.parse(f"# c\n{text}\n")
            self.assertIn("line 2", str(caught.exception))
            self.assertIn(expected, str(caught.exception))

    def test_limit_after_step_is_rejected(self) -> None:
        with self.assertRaises(scn.ScenarioError):
            scn.parse("sleep 1\nlimit time=5\n")

    def test_a_regex_with_an_equals_sign_is_an_argument(self) -> None:
        parsed = scn.parse('log "state=ready" timeout=5\n')
        self.assertEqual(parsed.steps[0].args, ["state=ready"])

    def test_a_mark_name_can_be_used_once(self) -> None:
        with self.assertRaisesRegex(scn.ScenarioError, "used twice"):
            scn.parse("mark a\nmark a\n")


class FakeTarget:
    """A target whose log, frames and pictures follow a script of ticks of the fake clock."""

    def __init__(self, clock: "FakeClock", logs: list[str] | None = None,
                 pictures: list[bytes] | None = None, frames: list[int] | None = None,
                 alive_until: float = 1e9):
        self.clock = clock
        self.logs = logs or []
        self.pictures = pictures or []
        self.frames = frames or []
        self.alive_until = alive_until
        self.pad = None
        self.shots: list[str] = []
        self._log_at = 0

    def alive(self) -> bool:
        return self.clock.now < self.alive_until

    def log_tail(self) -> str:
        text = "".join(self.logs[self._log_at:self.clock.ticks + 1])
        self._log_at = self.clock.ticks + 1
        return text

    def frame_count(self) -> int:
        return self.frames[min(self.clock.ticks, len(self.frames) - 1)]

    def thumb(self) -> bytes | None:
        if not self.pictures:
            return None
        return self.pictures[min(self.clock.ticks, len(self.pictures) - 1)]

    def shot(self, name: str) -> None:
        self.shots.append(name)


class FakeClock:
    def __init__(self) -> None:
        self.now = 0.0
        self.ticks = 0

    def __call__(self) -> float:
        return self.now

    def sleep(self, seconds: float) -> None:
        self.now += seconds
        self.ticks += 1


def interpreter(target: FakeTarget, clock: FakeClock, events: list | None = None) -> scn.Interpreter:
    log = events if events is not None else []
    return scn.Interpreter(target, lambda kind, text: log.append((kind, text)), clock=clock,
                           sleep=clock.sleep)


def run(steps: str, target: FakeTarget, clock: FakeClock, events: list | None = None) -> None:
    interpreter(target, clock, events).run(scn.parse(steps))


GREY = bytes([128]) * 2304
BLACK = bytes(2304)
WHITE = bytes([255]) * 2304


class InterpreterTests(unittest.TestCase):
    def test_log_wait_finds_a_line_later_on(self) -> None:
        clock = FakeClock()
        target = FakeTarget(clock, logs=["foo\n", "bar\n", "marker here\n"])
        events: list = []
        run('log "marker h"\n', target, clock, events)
        self.assertEqual(events[-1], ("end", "scenario finished"))

    def test_log_wait_consumes_up_to_the_match(self) -> None:
        clock = FakeClock()
        target = FakeTarget(clock, logs=["one\ntwo\none\n"])
        run("log one\nlog two\nlog one\n", target, clock)
        clock2 = FakeClock()
        target2 = FakeTarget(clock2, logs=["one\n"])
        with self.assertRaises(scn.StepFailed):
            run("log one timeout=1\nlog one timeout=1\n", target2, clock2)

    def test_log_wait_handles_a_line_split_between_reads(self) -> None:
        clock = FakeClock()
        target = FakeTarget(clock, logs=["mark", "er\n"])
        run("log marker timeout=2\n", target, clock)

    def test_log_wait_sees_a_line_logged_just_before_exit(self) -> None:
        clock = FakeClock()
        target = FakeTarget(clock, logs=["last words\n"], alive_until=0.0)
        run("log last timeout=1\n", target, clock)

    def test_log_match_ending_at_the_newline_consumes_the_line(self) -> None:
        clock = FakeClock()
        target = FakeTarget(clock, logs=["foo\n"])
        with self.assertRaises(scn.StepFailed):
            run("log 'foo\\n' timeout=1\nlog foo timeout=1\n", target, clock)

    def test_log_anchors_apply_per_line(self) -> None:
        clock = FakeClock()
        target = FakeTarget(clock, logs=["other\nLoaded now\n"])
        run('log "^Loaded" timeout=1\n', target, clock)

    def test_log_ignores_an_unfinished_line(self) -> None:
        clock = FakeClock()
        target = FakeTarget(clock, logs=["ready_for"])
        with self.assertRaises(scn.StepFailed):
            run('log "ready" timeout=1\n', target, clock)

    def test_log_timeout_fails_with_the_step(self) -> None:
        clock = FakeClock()
        target = FakeTarget(clock, logs=["nothing\n"])
        with self.assertRaises(scn.StepFailed) as caught:
            run("log missing timeout=1\n", target, clock)
        self.assertIn("log missing", str(caught.exception))

    def test_target_exit_fails_a_wait(self) -> None:
        clock = FakeClock()
        target = FakeTarget(clock, logs=[""], alive_until=0.5)
        with self.assertRaises(scn.StepFailed) as caught:
            run("log never timeout=10\n", target, clock)
        self.assertIn("exited", str(caught.exception))

    def test_frames_counts_from_the_start_of_the_step(self) -> None:
        clock = FakeClock()
        target = FakeTarget(clock, frames=[100, 100, 103, 110])
        run("frames 5\n", target, clock)
        self.assertEqual(clock.ticks, 3)

    def test_nonblack_waits_for_the_level(self) -> None:
        clock = FakeClock()
        target = FakeTarget(clock, pictures=[BLACK, BLACK, GREY])
        run("nonblack level=100\n", target, clock)
        self.assertEqual(clock.ticks, 2)

    def test_changed_compares_with_the_first_picture(self) -> None:
        clock = FakeClock()
        target = FakeTarget(clock, pictures=[BLACK, BLACK, GREY])
        run("changed delta=50\n", target, clock)
        self.assertEqual(clock.ticks, 2)

    def test_stable_restarts_when_the_picture_changes(self) -> None:
        clock = FakeClock()
        target = FakeTarget(clock, pictures=[BLACK, GREY, GREY, GREY, GREY, GREY])
        events: list = []
        run("stable seconds=1 delta=5\n", target, clock, events)
        self.assertEqual(clock.now, 1.25)  # the change at 0.25, then a full second unchanged
        self.assertIn(("stable", "unchanged for 1.000 s"), events)

    def test_stable_fails_on_constant_motion(self) -> None:
        clock = FakeClock()
        pictures = [BLACK if i % 2 else WHITE for i in range(100)]
        target = FakeTarget(clock, pictures=pictures)
        with self.assertRaises(scn.StepFailed):
            run("stable seconds=2 timeout=3\n", target, clock)

    def test_picture_wait_fails_without_frames(self) -> None:
        clock = FakeClock()
        target = FakeTarget(clock)
        with self.assertRaises(scn.StepFailed):
            run("nonblack timeout=2\n", target, clock)

    def test_shot_and_mark(self) -> None:
        clock = FakeClock()
        target = FakeTarget(clock)
        events: list = []
        run("shot first\nmark menu\n", target, clock, events)
        self.assertEqual(target.shots, ["first"])
        self.assertIn(("mark", "menu"), events)

    def test_input_without_a_pad_fails(self) -> None:
        clock = FakeClock()
        target = FakeTarget(clock)
        with self.assertRaises(scn.StepFailed) as caught:
            run("tap A\n", target, clock)
        self.assertIn("no gamepad", str(caught.exception))

    def test_tap_repeats_with_the_gap(self) -> None:
        clock = FakeClock()
        target = FakeTarget(clock)
        tapped: list[tuple[str, int]] = []
        target.pad = type("Pad", (), {"tap": lambda self, n, ms: tapped.append((n, ms))})()
        run("tap A ms=30 count=3 gap=2\n", target, clock)
        self.assertEqual(tapped, [("A", 30)] * 3)
        self.assertEqual(clock.now, 4)


if __name__ == "__main__":
    unittest.main()
