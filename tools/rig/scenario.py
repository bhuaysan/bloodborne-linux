"""Scenario scripts of the test rig: a text format, its parser and its interpreter.

A scenario is a list of steps, one per line, `#` starts a comment. Options are key=value.

  limit [time=SECONDS] [memory=SIZE]   defaults for the run, before the first step
  tap BUTTON [ms=120] [count=1] [gap=0.3]    A B X Y LB RB BACK START GUIDE L3 R3
  dpad U|D|L|R [ms=120] [count=1] [gap=0.3]
  trigger L|R [ms=200]
  stick LX|LY|RX|RY VALUE
  neutral
  sleep SECONDS
  log REGEX [timeout=60]               a new complete line of the target's log matches (quote the
                                       regex in single quotes when it has backslashes or a #)
  frames N [timeout=60]                N more presents happened
  nonblack [level=16] [timeout=60]     the picture's mean luma reaches level
  changed [delta=4] [timeout=60]       the picture differs from the one at the start of the step
  stable [seconds=2] [delta=2] [timeout=60]    the picture stops changing for that long; logs
                                       a "stable" event with how long it has been unchanged
  shot NAME                            save the current picture
  mark NAME                            start a named segment of the frame-time report

A wait that runs out of time fails the scenario. The picture conditions need a target that
presents frames. `log` needs only its log, so a target without a picture can run those.
"""

from __future__ import annotations

import math
import re
import shlex
import time
from collections.abc import Callable
from dataclasses import dataclass, field
from typing import Protocol

BUTTONS = frozenset("A B X Y LB RB BACK START GUIDE L3 R3".split())
SIDES = frozenset("LR")
DIRECTIONS = frozenset("UDLR")
AXES = frozenset("LX LY RX RY".split())
SHOT_NAME = re.compile(r"[A-Za-z0-9_-]{1,64}")

_DEFAULT_TIMEOUT = 60.0


class ScenarioError(Exception):
    pass


@dataclass
class Step:
    op: str
    args: list[str]
    opts: dict[str, str]
    line: int

    def text(self) -> str:
        opts = " ".join(f"{k}={v}" for k, v in self.opts.items())
        return " ".join([self.op, *self.args, opts]).strip()

    def number(self, key: str, default: float) -> float:
        return float(self.opts.get(key, default))

    def count(self) -> int:
        return int(self.opts.get("count", 1))


@dataclass
class Scenario:
    steps: list[Step] = field(default_factory=list)
    time_limit: int | None = None
    memory_limit: str | None = None

    def uses_pad(self) -> bool:
        return any(s.op in ("tap", "dpad", "trigger", "stick", "neutral") for s in self.steps)

    def uses_frames(self) -> bool:
        return any(s.op in ("frames", "nonblack", "changed", "stable", "shot") for s in self.steps)


# op: (positional args, allowed options)
_SPEC: dict[str, tuple[int, set[str]]] = {
    "tap": (1, {"ms", "count", "gap"}),
    "dpad": (1, {"ms", "count", "gap"}),
    "trigger": (1, {"ms"}),
    "stick": (2, set()),
    "neutral": (0, set()),
    "sleep": (1, set()),
    "log": (1, {"timeout"}),
    "frames": (1, {"timeout"}),
    "nonblack": (0, {"level", "timeout"}),
    "changed": (0, {"delta", "timeout"}),
    "stable": (0, {"seconds", "delta", "timeout"}),
    "shot": (1, set()),
    "mark": (1, set()),
}


def _finite(text: str, what: str) -> float:
    value = float(text)
    if not math.isfinite(value):
        raise ValueError(f"{what} must be a finite number")
    return value


def _check_step(step: Step) -> None:
    head = step.args[0] if step.args else ""
    if step.op == "tap" and head not in BUTTONS:
        raise ValueError(f"unknown button {head}")
    if step.op == "dpad" and head not in DIRECTIONS:
        raise ValueError(f"unknown direction {head}")
    if step.op == "trigger" and head not in SIDES:
        raise ValueError(f"unknown trigger {head}")
    if step.op == "stick":
        if head not in AXES:
            raise ValueError(f"unknown axis {head}")
        if not -32768 <= int(step.args[1]) <= 32767:
            raise ValueError("stick value out of range")
    if step.op == "sleep" and _finite(head, "sleep") < 0:
        raise ValueError("negative sleep")
    if step.op == "frames" and int(head) < 1:
        raise ValueError("frame count must be positive")
    if step.op == "log":
        re.compile(head, re.MULTILINE)
    if step.op in ("shot", "mark") and not SHOT_NAME.fullmatch(head):
        raise ValueError(f"bad name {head!r}: 1 to 64 of [A-Za-z0-9_-]")
    for key in ("timeout", "delta", "level", "seconds", "gap"):
        if key in step.opts and _finite(step.opts[key], key) < 0:
            raise ValueError(f"negative {key}")
    if "ms" in step.opts and int(step.opts["ms"]) < 0:
        raise ValueError("negative ms")
    if "count" in step.opts and int(step.opts["count"]) < 1:
        raise ValueError("count must be positive")


def parse(text: str) -> Scenario:
    scenario = Scenario()
    marks: set[str] = set()
    for number, raw in enumerate(text.splitlines(), 1):
        try:
            tokens = shlex.split(raw, comments=True)
            if not tokens:
                continue
            op, rest = tokens[0], tokens[1:]
            if op != "limit" and op not in _SPEC:
                raise ValueError(f"unknown step {op}")
            want, allowed = _SPEC.get(op, (0, {"time", "memory"}))
            # Only this step's own option names are options, so a regex like "state=ready" is
            # an argument.
            opts: dict[str, str] = {}
            args: list[str] = []
            for token in rest:
                key, sep, value = token.partition("=")
                if sep and key in allowed:
                    opts[key] = value
                else:
                    args.append(token)
            if op == "limit":
                if scenario.steps:
                    raise ValueError("limit must come before the first step")
                if args:
                    raise ValueError("limit takes time= and memory=")
                if "time" in opts:
                    scenario.time_limit = int(opts["time"])
                    if scenario.time_limit <= 0:
                        raise ValueError("limit time must be positive")
                if "memory" in opts:
                    if not re.fullmatch(r"\d+[KMGT]?", opts["memory"]):
                        raise ValueError("limit memory must look like 12G")
                    scenario.memory_limit = opts["memory"]
                continue
            if len(args) != want:
                raise ValueError(f"{op} takes {want} argument(s), got {len(args)}")
            step = Step(op, args, opts, number)
            _check_step(step)
            if step.op == "mark":
                if step.args[0] in marks:
                    raise ValueError(f"mark {step.args[0]} is used twice")
                marks.add(step.args[0])
            scenario.steps.append(step)
        except (ValueError, re.error) as e:
            raise ScenarioError(f"line {number}: {e}") from e
    return scenario


class Pad(Protocol):
    def tap(self, name: str, ms: int) -> None: ...
    def dpad(self, direction: str, ms: int) -> None: ...
    def trigger(self, side: str, ms: int) -> None: ...
    def stick(self, axis: str, value: int) -> None: ...
    def neutral(self) -> None: ...


class Target(Protocol):
    """What the interpreter needs from a running program."""

    pad: Pad | None

    def alive(self) -> bool: ...
    def log_tail(self) -> str:
        """Text the target logged since the last call."""
    def frame_count(self) -> int: ...
    def thumb(self) -> bytes | None:
        """A fresh small grey picture, or None when no frame arrived in time."""
    def shot(self, name: str) -> None: ...


class StepFailed(Exception):
    pass


def difference(a: bytes, b: bytes) -> float:
    """Mean absolute difference between two pictures of the same size, 0 to 255."""
    return sum(abs(x - y) for x, y in zip(a, b, strict=True)) / len(a)


def mean_level(picture: bytes) -> float:
    return sum(picture) / len(picture)


@dataclass
class Interpreter:
    target: Target
    event: Callable[[str, str], None]  # (kind, text)
    poll: float = 0.25
    clock: Callable[[], float] = time.monotonic
    sleep: Callable[[float], None] = time.sleep
    _log_buffer: str = ""

    def run(self, scenario: Scenario) -> None:
        for step in scenario.steps:
            self.event("step", f"{step.line}: {step.text()}")
            self._step(step)
        self.event("end", "scenario finished")

    def _check_alive(self) -> None:
        if not self.target.alive():
            raise StepFailed("the target exited")

    def _pad(self) -> Pad:
        if self.target.pad is None:
            raise StepFailed("this run has no gamepad")
        return self.target.pad

    def _repeat(self, step: Step, action: Callable[[], None]) -> None:
        for i in range(step.count()):
            if i:
                self.sleep(step.number("gap", 0.3))
            self._check_alive()
            action()

    def _step(self, step: Step) -> None:
        if step.op != "log":  # a log wait still reads what a target wrote before it exited
            self._check_alive()
        head = step.args[0] if step.args else ""
        match step.op:
            case "tap":
                ms = int(step.number("ms", 120))
                self._repeat(step, lambda: self._pad().tap(head, ms))
            case "dpad":
                ms = int(step.number("ms", 120))
                self._repeat(step, lambda: self._pad().dpad(head, ms))
            case "trigger":
                self._pad().trigger(head, int(step.number("ms", 200)))
            case "stick":
                self._pad().stick(head, int(step.args[1]))
            case "neutral":
                self._pad().neutral()
            case "sleep":
                self._sleep_alive(float(head))
            case "log":
                self._wait_log(re.compile(head, re.MULTILINE), step)
            case "frames":
                self._wait_frames(int(head), step)
            case "nonblack":
                self._wait_nonblack(step)
            case "changed":
                self._wait_changed(step)
            case "stable":
                self._wait_stable(step)
            case "shot":
                self.target.shot(head)
            case "mark":
                self.event("mark", head)

    def _sleep_alive(self, seconds: float) -> None:
        end = self.clock() + seconds
        while (left := end - self.clock()) > 0:
            self.sleep(min(left, self.poll))
            self._check_alive()

    def _deadline(self, step: Step) -> float:
        return self.clock() + step.number("timeout", _DEFAULT_TIMEOUT)

    def _tick(self, step: Step, deadline: float) -> None:
        self._check_alive()
        if self.clock() >= deadline:
            raise StepFailed(f"timed out: {step.text()}")
        self.sleep(self.poll)

    def _wait_log(self, pattern: re.Pattern[str], step: Step) -> None:
        deadline = self._deadline(step)
        while True:
            # Read before the liveness check in _tick, so a target that logs the line and then
            # exits still counts.
            alive = self.target.alive()
            self._log_buffer += self.target.log_tail()
            complete = self._log_buffer.rfind("\n") + 1
            match = pattern.search(self._log_buffer, 0, complete)
            if match:
                # Consume up to the end of the matching line, so the next wait starts after it.
                end = max(match.start(), match.end() - 1)
                self._log_buffer = self._log_buffer[self._log_buffer.find("\n", end) + 1:]
                return
            # Keep only the unfinished last line, so the buffer stays small over a long run.
            self._log_buffer = self._log_buffer[complete:]
            if not alive:
                raise StepFailed("the target exited")
            self._tick(step, deadline)

    def _wait_frames(self, count: int, step: Step) -> None:
        deadline = self._deadline(step)
        goal = self.target.frame_count() + count
        while self.target.frame_count() < goal:
            self._tick(step, deadline)

    def _picture(self, step: Step, deadline: float) -> bytes:
        while True:
            picture = self.target.thumb()
            if picture is not None:
                return picture
            self._tick(step, deadline)

    def _wait_nonblack(self, step: Step) -> None:
        deadline = self._deadline(step)
        level = step.number("level", 16)
        while mean_level(self._picture(step, deadline)) < level:
            self._tick(step, deadline)

    def _wait_changed(self, step: Step) -> None:
        deadline = self._deadline(step)
        reference = self._picture(step, deadline)
        delta = step.number("delta", 4)
        while difference(reference, self._picture(step, deadline)) < delta:
            self._tick(step, deadline)

    def _wait_stable(self, step: Step) -> None:
        deadline = self._deadline(step)
        seconds = step.number("seconds", 2)
        delta = step.number("delta", 2)
        reference = self._picture(step, deadline)
        since = self.clock()
        while self.clock() - since < seconds:
            self._tick(step, deadline)
            current = self._picture(step, deadline)
            if difference(reference, current) > delta:
                reference, since = current, self.clock()
        # When the picture last changed: the rig's time to a menu is measured from it.
        self.event("stable", f"unchanged for {self.clock() - since:.3f} s")
