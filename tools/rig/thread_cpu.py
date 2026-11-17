#!/usr/bin/env python3
"""CPU time per presented frame of each thread of a run, per scenario segment.

The rig samples the user and system time of every thread of the target once a second into
`threads.csv` (`ThreadSampler` in rig.py). This takes the samples nearest each segment's marks
and divides each thread's CPU time between them by the presents in the same span. Unlike the frame
counters of `--log profile=debug`, it costs the target nothing and counts kernel time too, and
unlike `perf`, it sees threads that sleep as idle and not as missing (docs/test-rig.md).

    thread_cpu.py RUN [--segment GLOB] [--thread GLOB] [--top N]
"""
from __future__ import annotations

import argparse
import bisect
import csv
import fnmatch
import re
import sys
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import frame_profile  # noqa: E402
import report  # noqa: E402

TICK_MS = 10  # /proc/<pid>/task/<tid>/stat counts clock ticks of 1/100 s (USER_HZ)


@dataclass
class ThreadCpu:
    name: str      # thread name; numbered siblings (GXWorker:1 ...) are summed under one name
    threads: int
    cpu_ms: float  # user and system time per presented frame
    sys_ms: float  # its system time


def read_threads(path: Path) -> dict[int, dict[int, tuple[str, int, int]]]:
    """threads.csv as {mono_ns: {tid: (name, utime, stime)}}, ticks as the kernel counts them."""
    samples: dict[int, dict[int, tuple[str, int, int]]] = defaultdict(dict)
    if not path.exists():
        return {}
    with path.open(newline="") as f:
        for row in csv.DictReader(f):
            try:
                samples[int(row["mono_ns"])][int(row["tid"])] = (
                    row["name"], int(row["utime"]), int(row["stime"]))
            except (KeyError, TypeError, ValueError):
                continue  # a run killed mid-write can leave a truncated last line
    return dict(samples)


def group_name(name: str) -> str:
    """GXWorker:3 and HavokWorkerThread5 count as one name; GXRenderThread stays itself."""
    return re.sub(r"[:#]?\d+$", "", name) or name


def segment_cpu(samples: dict[int, dict[int, tuple[str, int, int]]], presents: list[int],
                start_ns: int, end_ns: int) -> tuple[float, int, list[ThreadCpu]]:
    """(span in s, presents, threads by CPU time) between the samples nearest start and end."""
    times = sorted(samples)
    if len(times) < 2:
        return 0.0, 0, []

    def at_or_before(t: int) -> int:
        return times[max(bisect.bisect_right(times, t) - 1, 0)]

    first, last = at_or_before(start_ns), at_or_before(end_ns)
    if last <= first:
        return 0.0, 0, []
    frames = sum(1 for p in presents if first <= p < last)
    if frames == 0:
        return (last - first) / 1e9, 0, []
    totals: dict[str, list[int]] = defaultdict(lambda: [0, 0, 0])
    for tid, (name, utime, stime) in samples[last].items():
        before = samples[first].get(tid)
        if before is None:
            continue  # started inside the span: a short-lived thread, not a frame's work
        group = totals[group_name(name)]
        group[0] += utime + stime - before[1] - before[2]
        group[1] += stime - before[2]
        group[2] += 1
    result = [ThreadCpu(name, n, cpu * TICK_MS / frames, sys_ * TICK_MS / frames)
              for name, (cpu, sys_, n) in totals.items()]
    result.sort(key=lambda t: -t.cpu_ms)
    return (last - first) / 1e9, frames, result


def run_report(run: Path, segment: str = "*", thread: str = "*", top: int = 6) -> str:
    samples = read_threads(run / "threads.csv")
    if not samples:
        return "no threads.csv: the run is older than the thread sampler\n"
    marks = frame_profile.read_marks(run / "events.csv")
    presents = [f.present_ns for f in report.read_frames(run / "frames.csv")]
    # The last segment ends with the scenario: samples after it see the target shutting down.
    with (run / "events.csv").open(newline="") as f:
        end = max((int(r["mono_ns"]) for r in csv.DictReader(f)), default=max(samples))
    lines = []
    for (start, name), (stop, _) in zip(marks, [*marks[1:], (end, "")]):
        if not fnmatch.fnmatch(name, segment):
            continue
        span, frames, threads = segment_cpu(samples, presents, start, stop)
        lines.append(f"{name}: {span:.1f} s, {frames} presents (CPU ms per present, sys inside)")
        shown = [t for t in threads if fnmatch.fnmatch(t.name, thread)][:top]
        for t in shown:
            count = f" x{t.threads}" if t.threads > 1 else ""
            lines.append(f"  {t.cpu_ms:7.2f} ({t.sys_ms:5.2f})  {t.name}{count}")
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("run", type=Path)
    parser.add_argument("--segment", default="*", help="glob: only the segments with such a name")
    parser.add_argument("--thread", default="*",
                        help="glob on the thread name, e.g. 'shadPS4:GpuComm' for the GPU thread")
    parser.add_argument("--top", type=int, default=6, help="threads to list per segment")
    args = parser.parse_args()
    print(run_report(args.run, args.segment, args.thread, args.top), end="")
    return 0


if __name__ == "__main__":
    sys.exit(main())
