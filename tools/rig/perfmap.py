#!/usr/bin/env python3
"""Writes /tmp/perf-<pid>.map, the file perf reads to name code outside any file: the guest's
functions from symbols/functions.csv, at the eboot's load base as bb-run maps it
(loader::default_base). Then `perf report -i RUN/perf.data` shows their names.

    perfmap.py --perf-data RUN/perf.data     # the pid is the one perf recorded
    perfmap.py --pid PID [--out DIR]

symbols/functions.csv holds the functions we named so far, so most guest samples stay
`[unknown]` and show as raw addresses; they are above 0x10000000 and below the first library.
"""
from __future__ import annotations

import argparse
import csv
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LOAD_BASE = 0x10000000  # src/loader/image.hpp default_base
MAX_SIZE = 0x10000  # functions.csv has no sizes: a function ends at the next one or after this


def perf_map_lines(csv_path: Path, base: int = LOAD_BASE) -> list[str]:
    with csv_path.open(newline="") as f:
        functions = sorted((int(r["address"], 16), r["name"]) for r in csv.DictReader(f))
    lines = []
    for i, (address, name) in enumerate(functions):
        size = MAX_SIZE
        if i + 1 < len(functions):
            size = min(size, functions[i + 1][0] - address)
        lines.append(f"{base + address:x} {size:x} {name}")
    return lines


def recorded_pid(perf_data: Path) -> int:
    out = subprocess.run(["perf", "script", "-i", str(perf_data), "-F", "pid"], check=True,
                         capture_output=True, text=True).stdout
    pids = [int(p) for p in out.split()]
    if not pids:
        raise SystemExit(f"no samples in {perf_data}")
    return max(set(pids), key=pids.count)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    which = parser.add_mutually_exclusive_group(required=True)
    which.add_argument("--perf-data", type=Path)
    which.add_argument("--pid", type=int)
    parser.add_argument("--out", type=Path, default=Path("/tmp"), help="perf reads only /tmp")
    parser.add_argument("--symbols", type=Path, default=ROOT / "symbols" / "functions.csv")
    args = parser.parse_args()
    pid = args.pid or recorded_pid(args.perf_data)
    path = args.out / f"perf-{pid}.map"
    path.write_text("\n".join(perf_map_lines(args.symbols)) + "\n")
    print(path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
