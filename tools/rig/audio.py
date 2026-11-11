#!/usr/bin/env python3
"""Sound level of a run, from SDL's disk audio driver.

`rig.py run ... --audio` starts the target with SDL_AUDIO_DRIVER=disk, which writes what the
program plays to a file in real time, in the format the program opened (both Bloodborne targets
open 48000 Hz, 8 channels, 32-bit float). This module reads such a file in chunks and gives the
RMS level and the peak per window, and the RMS over a span, so that a run of ours can be held
against the reference's (docs/test-rig.md).

  audio.py FILE [FILE ...] [--seconds N]       levels of each file over its first N seconds
"""

from __future__ import annotations

import argparse
import array
import json
import math
import sys
from pathlib import Path

RATE = 48000
CHANNELS = 8
WINDOW_S = 10
# One chunk is one window, so no file is ever read whole. A float is 4 bytes.
BYTES_PER_SECOND = RATE * CHANNELS * 4


def windows(path: Path, seconds: float | None = None, channels: int = CHANNELS,
            rate: int = RATE, window_s: int = WINDOW_S):
    """Yield (start second, rms, peak, sample count) per window, the last one may be short."""
    frame_bytes = channels * 4
    limit = None if seconds is None else int(seconds * rate) * frame_bytes
    window_bytes = window_s * rate * frame_bytes
    done = 0
    with open(path, "rb") as f:
        while limit is None or done < limit:
            want = window_bytes if limit is None else min(window_bytes, limit - done)
            data = f.read(want - want % frame_bytes)
            if not data:
                return
            samples = array.array("f")
            samples.frombytes(data[:len(data) - len(data) % 4])
            if sys.byteorder == "big":
                samples.byteswap()
            squares = math.fsum(s * s for s in samples)
            yield (done // frame_bytes // rate,
                   math.sqrt(squares / len(samples)) if samples else 0.0,
                   max((abs(s) for s in samples), default=0.0), len(samples))
            done += len(data)


def summary(path: Path, seconds: float | None = None) -> dict:
    """RMS over the span (windows weighted by their sample count), the peak, the silent windows."""
    energy, count, peak, silent, total, levels = 0.0, 0, 0.0, 0, 0, []
    for _, rms, top, n in windows(path, seconds):
        energy += rms * rms * n
        levels.append(round(rms, 5))
        count += n
        peak = max(peak, top)
        silent += rms < 1e-4
        total += 1
    return {"windows": total, "silent_windows": silent,
            "rms": round(math.sqrt(energy / count), 6) if count else 0.0,
            "peak": round(peak, 4), "levels": levels}


def ratio(ours: dict, reference: dict) -> float | None:
    """Our RMS over the reference's, or None when the reference is silent."""
    return ours["rms"] / reference["rms"] if reference["rms"] > 0 else None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("files", nargs="+", type=Path)
    parser.add_argument("--seconds", type=float, help="only the first N seconds")
    args = parser.parse_args(argv)
    print(json.dumps({str(p): summary(p, args.seconds) for p in args.files}, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
