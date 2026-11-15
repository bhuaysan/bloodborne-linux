#!/usr/bin/env python3
"""Where the slow frames of a bb-run run go, from the frame counters of its log.

Run bb-run with `--log profile=debug` (rig.py run bb-run SCENARIO --log profile=debug). Once per
frame (the guest's submit-done) its log gets a line `Frame profile: at NS ns, US us since the
last, part=US us/CALLS, ...` (src/common/frame_profile.hpp). This reads them, together with the
`Pipeline build` and `Pipeline wait` lines, and for each scenario segment names what the slowest
frames spent on top of what a normal frame does.

    frame_profile.py RUN [--segment GLOB] [--top N] [--slow FACTOR]
"""
from __future__ import annotations

import argparse
import bisect
import csv
import fnmatch
import re
import statistics
import sys
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import report  # noqa: E402

LINE = re.compile(r"Frame profile: at (\d+) ns, (\d+) us since the last((?:, \w+=\d+ us/\d+)*)")
UPLOAD_BURST = 50  # refreshes in one frame; a normal frame has about 5
PART = re.compile(r", (\w+)=(\d+) us/(\d+)")


@dataclass
class GpuFrame:
    at_ns: int
    interval_us: int  # since the previous line, 0 for the first
    us: dict[str, int] = field(default_factory=dict)  # part -> time (inclusive)
    calls: dict[str, int] = field(default_factory=dict)


def read_profile(log: str) -> list[GpuFrame]:
    frames = []
    for m in LINE.finditer(log):
        frame = GpuFrame(int(m[1]), int(m[2]))
        for part in PART.finditer(m[3]):
            frame.us[part[1]] = int(part[2])
            frame.calls[part[1]] = int(part[3])
        frames.append(frame)
    return frames


def read_marks(events: Path) -> list[tuple[int, str]]:
    with events.open(newline="") as f:
        return [(int(r["mono_ns"]), r["text"]) for r in csv.DictReader(f) if r["kind"] == "mark"]


def add_builds(frames: list[GpuFrame], builds: list[report.Build]) -> None:
    """The time of pipeline builds and waits that overlap a frame's span goes into its `build`."""
    for frame in frames:
        end = frame.at_ns
        start = end - frame.interval_us * 1000
        total = sum(min(b.start_ns + b.duration_ns, end) - max(b.start_ns, start)
                    for b in builds if b.start_ns < end and b.start_ns + b.duration_ns > start)
        if total > 0:
            frame.us["build"] = total // 1000


def _mean(values: list[int]) -> float:
    return statistics.fmean(values) if values else 0.0


def analyse(frames: list[GpuFrame], factor: float = 1.5, top: int = 5) -> dict | None:
    """The slow frames of one segment: those over `factor` times the median interval. None without
    at least a few intervals. `excess` is, per part, the mean over the slow frames minus the mean
    over the others, largest first."""
    frames = [f for f in frames if f.interval_us > 0]
    if len(frames) < 5:
        return None
    intervals = sorted(f.interval_us for f in frames)
    median = statistics.median(intervals)
    slow = [f for f in frames if f.interval_us > factor * median]
    normal = [f for f in frames if f.interval_us <= factor * median]
    parts = sorted({p for f in frames for p in f.us})
    excess = {p: _mean([f.us.get(p, 0) for f in slow]) - _mean([f.us.get(p, 0) for f in normal])
              for p in parts}
    excess["interval"] = _mean([f.interval_us for f in slow]) - _mean([f.interval_us for f in normal])
    return {
        "frames": len(frames),
        "median_us": median,
        "p95_us": intervals[max(0, -(-len(intervals) * 95 // 100) - 1)],
        "max_us": intervals[-1],
        "slow": len(slow),
        "slowest": sorted(slow, key=lambda f: -f.interval_us)[:top],
        "excess_us": dict(sorted(excess.items(), key=lambda kv: -kv[1])),
        "normal_us": {p: _mean([f.us.get(p, 0) for f in normal]) for p in parts},
        "upload_bursts": [(f.calls["upload"], f.calls.get("upload_new", 0))
                          for f in frames if f.calls.get("upload", 0) > UPLOAD_BURST],
    }


def late_presents(frames: list[GpuFrame], presents: list[report.Frame],
                  factor: float = 1.5) -> dict | None:
    """What the late presents have that the on-time ones do not. A present is late when its
    interval is over `factor` times the median; it is matched to the GPU frame whose submit-done
    came last before it. `lag_ms` is the time from that submit-done to the present, `commands_ms`
    what the GPU thread spent on that frame's commands. None without late presents."""
    rows = []  # (late, lag ms, commands ms)
    ats = [f.at_ns for f in frames]
    intervals = report.intervals_ms(presents)
    if not intervals or not frames:
        return None
    limit = factor * statistics.median(intervals)
    for previous, current in zip(presents, presents[1:]):
        i = bisect.bisect_right(ats, current.present_ns) - 1
        if i < 0 or previous.captured or current.captured:
            continue
        rows.append(((current.present_ns - previous.present_ns) / 1e6 > limit,
                     (current.present_ns - ats[i]) / 1e6, frames[i].us.get("commands", 0) / 1e3))
    late = [r for r in rows if r[0]]
    if not late:
        return None
    on_time = [r for r in rows if not r[0]]
    return {"late": len(late), "presents": len(rows),
            "lag_ms": statistics.fmean(r[1] for r in late),
            "on_time_lag_ms": statistics.fmean(r[1] for r in on_time) if on_time else 0.0,
            "commands_ms": statistics.fmean(r[2] for r in late),
            "on_time_commands_ms": statistics.fmean(r[2] for r in on_time) if on_time else 0.0}


def format_segment(name: str, result: dict | None, late: dict | None = None) -> str:
    if result is None:
        return f"{name}: too few frames\n"
    out = [f"{name}: {result['frames']} frames, interval p50 {result['median_us'] / 1000:.1f} ms, "
           f"p95 {result['p95_us'] / 1000:.1f} ms, max {result['max_us'] / 1000:.1f} ms, "
           f"{result['slow']} slow (over the factor times the median)"]
    if result["slow"]:
        out.append("  slowest frames (ms; parts are inclusive, `build` is pipeline build and wait time):")
        for f in result["slowest"]:
            costs = ", ".join(f"{p} {us / 1000:.1f}" for p, us in
                              sorted(f.us.items(), key=lambda kv: -kv[1])[:5])
            out.append(f"    {f.interval_us / 1000:6.1f}  {costs}")
        out.append("  extra cost of a slow frame over a normal one (ms, mean of each):")
        for part, us in list(result["excess_us"].items())[:6]:
            normal = result["normal_us"].get(part)
            tail = f" (normal frame: {normal / 1000:.2f})" if normal is not None else ""
            out.append(f"    {part:10} {us / 1000:+7.2f}{tail}")
    if result["normal_us"]:
        out.append("  a normal frame (ms, inclusive): " + ", ".join(
            f"{p} {us / 1000:.2f}" for p, us in
            sorted(result["normal_us"].items(), key=lambda kv: -kv[1])[:8]))
    if result["upload_bursts"]:
        out.append(f"  frames with over {UPLOAD_BURST} image refreshes: " + ", ".join(
            f"{n} ({new} new images)" for n, new in result["upload_bursts"]))
    if late:
        out.append(f"  late presents: {late['late']} of {late['presents']}; "
                   f"submit-done to present {late['lag_ms']:.1f} ms "
                   f"(on-time ones {late['on_time_lag_ms']:.1f}), commands of that frame "
                   f"{late['commands_ms']:.1f} ms (on-time {late['on_time_commands_ms']:.1f})")
    return "\n".join(out) + "\n"


def profile_run(run: Path, pattern: str = "*", factor: float = 1.5, top: int = 5) -> str:
    log = (run / "target.log").read_text(errors="replace")
    frames = read_profile(log)
    if not frames:
        return "no `Frame profile` lines: run bb-run with --log profile=debug\n"
    add_builds(frames, report.read_builds(log))
    marks = read_marks(run / "events.csv")
    presents = report.read_frames(run / "frames.csv")
    text = []
    for (start, name), (end, _) in zip(marks, [*marks[1:], (1 << 63, "")]):
        if fnmatch.fnmatch(name, pattern):
            in_segment = [f for f in frames if start <= f.at_ns < end]
            late = late_presents(in_segment, [p for p in presents if start <= p.present_ns < end],
                                 factor)
            text.append(format_segment(name, analyse(in_segment, factor, top), late))
    return "\n".join(text)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("run", type=Path)
    parser.add_argument("--segment", default="*", help="glob: only the segments with such a name")
    parser.add_argument("--top", type=int, default=5, help="slowest frames to list per segment")
    parser.add_argument("--slow", type=float, default=1.5,
                        help="a frame is slow above this factor times the segment's median")
    args = parser.parse_args()
    print(profile_run(args.run, args.segment, args.slow, args.top), end="")
    return 0


if __name__ == "__main__":
    sys.exit(main())
