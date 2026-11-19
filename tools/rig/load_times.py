#!/usr/bin/env python3
"""Load times of the rig's load scenarios (#655): from the press, or from YOU DIED, to the first
picture with the hunter under control.

  tools/rig/load_times.py RUN...

The runs come from `load-continue.scn` or `load-death-travel.scn` on either target. A load ends at
the first picture that shows the HUD's stamina bar again; the game hides the HUD on the title
menus, the loading pages and YOU DIED, and shows it as control comes back. The scenarios take one
picture a second (the rig's limit), so each end and the death's start are known within half a
second: the time between the picture before and the picture after the change, of which the
midpoint is reported.

The loads, by the marks of the scenarios:
- `menu` (Continue): from the A press after which the game starts its load. Both targets log a
  PlayGo call there (the reference by name, bb-run by NID), and the notices before Continue come
  in different numbers, so the press is the last one logged before that line.
- `death`: from the first picture without the HUD (YOU DIED) to the HUD at the lantern.
- `travel`: from the press after the mark (YES on "Return to the Hunter's Dream?") to the HUD in
  the Dream.
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import statistics
import sys
from dataclasses import dataclass
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import compare  # noqa: E402

# The start of a load from the title menu: libScePlayGo's scePlayGoSetInstallSpeed.
LOAD_MARKER = re.compile(rb"scePlayGoSetInstallSpeed|4AAcTU9R3XM: mode 1")
# The stamina bar in a 1280x720 picture: green pixels in this box (x0, y0, x1, y1).
STAMINA_BOX = (75, 56, 170, 60)
STAMINA_MIN = 0.2  # fraction of green pixels; 0.38 with the bar shown, 0 without


@dataclass(frozen=True)
class Load:
    name: str
    seconds: float
    uncertainty: float  # half the picture gaps around the end (and the start, for a death)


def stamina(picture: compare.Picture) -> float:
    """The fraction of the stamina bar's box that is green, scaled to a 1280x720 picture."""
    sx, sy = picture.width / 1280, picture.height / 720
    x0, y0, x1, y1 = STAMINA_BOX
    green = total = 0
    for y in range(int(y0 * sy), max(int(y1 * sy), int(y0 * sy) + 1)):
        for x in range(int(x0 * sx), int(x1 * sx)):
            i = (y * picture.width + x) * 3
            r, g = picture.rgb[i], picture.rgb[i + 1]
            green += g > 90 and g > 1.5 * r
            total += 1
    return green / total if total else 0.0


def read_events(run: Path) -> list[dict]:
    with (run / "events.csv").open(newline="") as f:
        return list(csv.DictReader(f))


def hud_pictures(run: Path, events: list[dict]) -> list[tuple[int, bool]]:
    """(mono_ns, HUD shown) of every shot of the run that was saved, in order."""
    result = []
    for e in events:
        if e["kind"] == "step" and re.search(r": shot \S+$", e["text"]):
            path = run / "shots" / f"{e['text'].split()[-1]}.png"
            if path.exists():
                result.append((int(e["mono_ns"]), stamina(compare.read_png(path)) >= STAMINA_MIN))
    return result


def _change(pictures: list[tuple[int, bool]], after: int, shown: bool) -> tuple[float, float] | None:
    """(midpoint ns, half gap ns) of the first change to `shown` after `after`. The last picture
    at or before `after` is the state it changes from."""
    previous = None
    for t, hud in pictures:
        if t > after and hud == shown and previous is not None and previous[1] != shown:
            return (previous[0] + t) / 2, (t - previous[0]) / 2
        previous = (t, hud)
    return None


def _first(pictures: list[tuple[int, bool]], after: int, shown: bool) -> int | None:
    return next((t for t, hud in pictures if t > after and hud == shown), None)


def _mark(events: list[dict], name: str) -> int | None:
    for i, e in enumerate(events):
        if e["kind"] == "mark" and e["text"] == name:
            return i
    return None


def _taps(events: list[dict], start: int) -> list[dict]:
    return [e for e in events[start:] if e["kind"] == "step" and re.search(r": tap A\b", e["text"])]


def loads(run: Path) -> list[Load]:
    events = read_events(run)
    pictures = hud_pictures(run, events)
    found = []
    menu = _mark(events, "menu")
    if menu is not None:
        log = (run / "target.log").read_bytes()
        marker = LOAD_MARKER.search(log, int(events[menu]["log_bytes"]))
        pressed = [e for e in _taps(events, menu) if marker and int(e["log_bytes"]) <= marker.start()]
        if pressed:
            start = int(pressed[-1]["mono_ns"])
            end = _change(pictures, start, True)
            if end:
                found.append(Load("continue", (end[0] - start) / 1e9, end[1] / 1e9))
    death = _mark(events, "death")
    if death is not None:
        died = _change(pictures, int(events[death]["mono_ns"]), False)
        end = died and _change(pictures, int(died[0]), True)
        if died and end:
            found.append(Load("death", (end[0] - died[0]) / 1e9, (end[1] + died[1]) / 1e9))
    travel = _mark(events, "travel")
    if travel is not None:
        taps = _taps(events, travel)
        if taps:
            start = int(taps[0]["mono_ns"])
            # The question hides the HUD and closing it may show it again for a moment, so the
            # load ends at the HUD after the first picture without it that follows the press.
            gone = _first(pictures, start, False)
            end = gone is not None and _change(pictures, gone - 1, True)
            if end:
                found.append(Load("travel", (end[0] - start) / 1e9, end[1] / 1e9))
    return found


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("runs", nargs="+", type=Path)
    args = parser.parse_args(argv)
    groups: dict[tuple[str, str, str], list[float]] = {}
    for run in args.runs:
        try:
            summary = json.loads((run / "summary.json").read_text())
            finished = (summary.get("scenario_ok", True) and (run / "events.csv").read_text()
                        .rstrip().endswith("end,scenario finished"))
            found = loads(run)
        except (OSError, ValueError, KeyError) as e:
            print(f"{run.name}: unreadable run ({e})")
            continue
        text = ", ".join(f"{x.name} {x.seconds:.1f} s (±{x.uncertainty:.1f})" for x in found)
        print(f"{run.name}: {text or 'no load found'}{'' if finished else ' (scenario not finished)'}")
        if finished:
            for x in found:
                groups.setdefault((summary["target"], summary["scenario"], x.name), []).append(x.seconds)
    print()
    for (target, scenario, name), values in sorted(groups.items()):
        print(f"{target:9s} {scenario:18s} {name:8s} median {statistics.median(values):5.1f} s, "
              f"spread {max(values) - min(values):4.1f} s, runs {len(values)}: "
              + " ".join(f"{v:.1f}" for v in values))
    return 0


if __name__ == "__main__":
    sys.exit(main())
