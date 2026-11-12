"""Compares our program's captures of a menu with the reference's, and its time to that menu.

  rig.py compare REF_RUN_1 REF_RUN_2 OUR_RUN [--shots 'menu-*.png']

The two reference runs give the noise floor: the mean difference between their captures. Ours
passes when its mean difference from the reference captures is at most twice that floor, and when
it reached the menu in at most twice the reference's time. docs/test-rig.md has the reasons.
"""

from __future__ import annotations

import csv
import json
import operator
import re
import struct
import zlib
from dataclasses import dataclass
from itertools import product
from pathlib import Path

# The tolerance the maintainer set (2026-11-11): within twice the reference's own noise floor, and
# within twice the reference's time to the menu.
PICTURE_FACTOR = 2.0
TIME_FACTOR = 2.0
DEFAULT_SHOTS = "menu-*.png"
_PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
_UNCHANGED = re.compile(r"unchanged for ([0-9.]+) s")


@dataclass(frozen=True)
class Picture:
    width: int
    height: int
    rgb: bytes  # rows of RGB bytes, no padding


def read_png(path: Path) -> Picture:
    """Reads a PNG as report.raw_to_png writes it: 8-bit RGB, not interlaced, filter 0 only."""
    data = path.read_bytes()
    if not data.startswith(_PNG_SIGNATURE):
        raise ValueError(f"{path}: not a PNG")
    pos, header, compressed = len(_PNG_SIGNATURE), None, bytearray()
    while pos + 8 <= len(data):
        length, tag = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if tag == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif tag == b"IDAT":
            compressed += body
        elif tag == b"IEND":
            break
        pos += 12 + length
    if header is None or header[2:] != (8, 2, 0, 0, 0):
        raise ValueError(f"{path}: only the rig's own 8-bit RGB pictures can be compared")
    width, height = header[0], header[1]
    rows = zlib.decompress(bytes(compressed))
    stride = width * 3 + 1
    if len(rows) != stride * height:
        raise ValueError(f"{path}: expected {stride * height} bytes of rows, got {len(rows)}")
    if any(rows[y * stride] for y in range(height)):
        raise ValueError(f"{path}: a row uses a PNG filter, which the rig never writes")
    rgb = b"".join(rows[y * stride + 1:(y + 1) * stride] for y in range(height))
    return Picture(width, height, rgb)


def difference(a: Picture, b: Picture) -> float:
    """Mean absolute difference over every channel of every pixel, 0 to 255."""
    if (a.width, a.height) != (b.width, b.height):
        raise ValueError(f"pictures of different sizes: {a.width}x{a.height} and "
                         f"{b.width}x{b.height}")
    return sum(map(abs, map(operator.sub, a.rgb, b.rgb))) / len(a.rgb)


def mean_difference(first: list[Picture], second: list[Picture]) -> float:
    """The mean of difference() over every pair with one picture from each list."""
    if not first or not second:
        raise ValueError("no pictures to compare")
    pairs = list(product(first, second))
    return sum(difference(a, b) for a, b in pairs) / len(pairs)


def time_to_stable(events: Path) -> float:
    """Seconds from the start of the target to the picture the first `stable` wait settled on."""
    start = stable = None
    with events.open(newline="") as f:
        for row in csv.DictReader(f):
            if row["kind"] == "start" and start is None:
                start = int(row["mono_ns"])
            elif row["kind"] == "stable" and stable is None:
                match = _UNCHANGED.fullmatch(row["text"])
                if not match:
                    raise ValueError(f"{events}: unreadable stable event {row['text']!r}")
                stable = int(row["mono_ns"]) - round(float(match.group(1)) * 1e9)
    if start is None or stable is None:
        raise ValueError(f"{events}: no start or no stable event")
    return (stable - start) / 1e9


def _ratio(value: float, base: float) -> float | None:
    return value / base if base else None


def verdict(reference_1: list[Picture], reference_2: list[Picture], ours: list[Picture],
            reference_times: list[float], our_time: float) -> dict:
    floor = mean_difference(reference_1, reference_2)
    our_difference = mean_difference(ours, reference_1 + reference_2)
    reference_time = sum(reference_times) / len(reference_times)
    pictures_ok = our_difference <= PICTURE_FACTOR * floor
    time_ok = our_time <= TIME_FACTOR * reference_time
    return {
        "noise_floor": round(floor, 4), "ours": round(our_difference, 4),
        "ratio": None if (r := _ratio(our_difference, floor)) is None else round(r, 3),
        "pictures_ok": pictures_ok,
        "reference_times_s": [round(t, 2) for t in reference_times],
        "our_time_s": round(our_time, 2),
        "time_ratio": None if (r := _ratio(our_time, reference_time)) is None else round(r, 3),
        "time_ok": time_ok,
        "ok": pictures_ok and time_ok,
    }


def run_scenario(run: Path) -> str:
    """The scenario of a run, from its summary.json; refuses a run whose scenario failed."""
    try:
        summary = json.loads((run / "summary.json").read_text())
    except (OSError, ValueError) as e:
        raise SystemExit(f"{run}: no readable summary.json: {e}") from e
    if summary.get("scenario_ok") is not True:
        raise SystemExit(f"{run}: the scenario did not finish ({summary.get('failure')})")
    return str(summary.get("scenario"))


def load_run(run: Path, shots: str) -> tuple[list[Picture], float]:
    paths = sorted((run / "shots").glob(shots))
    if not paths:
        raise SystemExit(f"{run}: no shots matching {shots}")
    return [read_png(p) for p in paths], time_to_stable(run / "events.csv")


def run_compare(reference_1: Path, reference_2: Path, ours: Path, shots: str) -> int:
    scenarios = {run_scenario(run) for run in (reference_1, reference_2, ours)}
    if len(scenarios) != 1:
        raise SystemExit(f"the runs are of different scenarios: {', '.join(sorted(scenarios))}")
    try:
        (r1, t1), (r2, t2), (o, t) = (load_run(run, shots)
                                      for run in (reference_1, reference_2, ours))
        if not len(r1) == len(r2) == len(o):
            raise SystemExit(f"the runs have different numbers of shots: reference_1 {len(r1)}, "
                             f"reference_2 {len(r2)}, ours {len(o)}")
        if mean_difference(r1, r2) == 0:
            raise SystemExit("the two reference runs are identical: the noise floor is 0, so "
                             "any difference would fail; the tolerance is not invented here")
        result = {"shots": {"reference_1": len(r1), "reference_2": len(r2), "ours": len(o)},
                  **verdict(r1, r2, o, [t1, t2], t)}
    except ValueError as e:
        raise SystemExit(str(e)) from e
    print(json.dumps(result, indent=2))
    return 0 if result["ok"] else 1
