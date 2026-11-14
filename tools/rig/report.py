"""What the test rig makes of a finished run: frame times, pictures, kernel faults, game dir check."""

from __future__ import annotations

import csv
import math
import os
import re
import struct
import zlib
from dataclasses import dataclass
from pathlib import Path

# Vulkan format numbers of the swapchain formats we can turn into pictures.
BGRA8 = {44, 50}  # B8G8R8A8_UNORM, _SRGB
RGBA8 = {37, 43}  # R8G8B8A8_UNORM, _SRGB
A2B10G10R10 = 64
A2R10G10B10 = 58

# The reference machine has an AMD GPU, so these are the amdgpu driver's messages.
KERNEL_FAULT = re.compile(r"amdgpu.*(fault|reset|recover|timeout|hang)|ring .* timeout", re.I)


def kernel_faults(journal: str) -> list[str]:
    return [line for line in journal.splitlines() if KERNEL_FAULT.search(line)]


def tree_state(root: Path) -> dict[str, tuple[int, int, int, int]]:
    """(size, mtime, ctime, mode) of every entry below root, keyed by relative path."""
    if not root.is_dir():
        raise NotADirectoryError(root)

    def fail(error: OSError) -> None:
        raise error

    state: dict[str, tuple[int, int, int, int]] = {}
    for directory, dirs, files in os.walk(root, onerror=fail):
        for name in [*dirs, *files]:
            path = Path(directory) / name
            info = path.lstat()
            state[str(path.relative_to(root))] = (info.st_size, info.st_mtime_ns, info.st_ctime_ns,
                                                  info.st_mode)
    return state


def tree_changes(before: dict, after: dict) -> list[str]:
    """Human-readable differences between two tree_state() results, empty when equal."""
    changes = [f"removed: {p}" for p in before.keys() - after.keys()]
    changes += [f"added: {p}" for p in after.keys() - before.keys()]
    changes += [f"changed: {p}" for p in before.keys() & after.keys() if before[p] != after[p]]
    return sorted(changes)


def _chunk(tag: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))


def _rgb_rows(pixels: bytes, width: int, height: int, vk_format: int) -> bytes:
    """Rows of RGB bytes, each with a leading PNG filter byte of 0."""
    rgb = bytearray(width * height * 3)
    if vk_format in BGRA8:
        rgb[0::3], rgb[1::3], rgb[2::3] = pixels[2::4], pixels[1::4], pixels[0::4]
    elif vk_format in RGBA8:
        rgb[0::3], rgb[1::3], rgb[2::3] = pixels[0::4], pixels[1::4], pixels[2::4]
    elif vk_format in (A2B10G10R10, A2R10G10B10):
        values = struct.unpack(f"<{width * height}I", pixels)
        red_shift, blue_shift = (0, 20) if vk_format == A2B10G10R10 else (20, 0)
        for i, value in enumerate(values):
            rgb[i * 3] = (value >> red_shift + 2) & 0xFF
            rgb[i * 3 + 1] = (value >> 12) & 0xFF
            rgb[i * 3 + 2] = (value >> blue_shift + 2) & 0xFF
    else:
        raise ValueError(f"unsupported swapchain format {vk_format}")
    stride = width * 3
    return b"".join(b"\0" + bytes(rgb[y * stride:(y + 1) * stride]) for y in range(height))


def raw_to_png(raw: Path, png: Path) -> None:
    """Writes a capture of the layer (see bb_rig_layer.cpp) as a PNG."""
    data = raw.read_bytes()
    end = data.index(b"\n")
    magic, width, height, vk_format = data[:end].split()
    if magic != b"BBRIGRAW1":
        raise ValueError(f"{raw}: not a capture")
    width, height = int(width), int(height)
    pixels = data[end + 1:]
    if len(pixels) != width * height * 4:
        raise ValueError(f"{raw}: expected {width * height * 4} bytes, got {len(pixels)}")
    rows = _rgb_rows(pixels, width, height, int(vk_format))
    png.write_bytes(b"\x89PNG\r\n\x1a\n"
                    + _chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
                    + _chunk(b"IDAT", zlib.compress(rows, 6))
                    + _chunk(b"IEND", b""))


@dataclass
class Frame:
    present_ns: int
    captured: bool


def read_frames(path: Path) -> list[Frame]:
    if not path.exists():
        return []
    with path.open(newline="") as f:
        frames = []
        for row in csv.DictReader(f):
            # A run killed mid-write can leave a truncated last line.
            if row.get("captured") in ("0", "1") and row["present_ns"].isdigit():
                frames.append(Frame(int(row["present_ns"]), row["captured"] == "1"))
    return frames


def intervals_ms(frames: list[Frame]) -> list[float]:
    """Time between presents in ms, leaving out the intervals a capture lengthened."""
    result = []
    for previous, current in zip(frames, frames[1:]):
        if not (previous.captured or current.captured):
            result.append((current.present_ns - previous.present_ns) / 1e6)
    return result


def _percentile(sorted_values: list[float], fraction: float) -> float:
    """Nearest-rank percentile."""
    return sorted_values[max(0, math.ceil(len(sorted_values) * fraction) - 1)]


def frame_stats(frames: list[Frame]) -> dict | None:
    values = sorted(intervals_ms(frames))
    if not values:
        return None
    median = _percentile(values, 0.5)
    return {
        "frames": len(frames),
        "fps": round(1000 / (sum(values) / len(values)), 2),
        "p50_ms": round(median, 2),
        "p95_ms": round(_percentile(values, 0.95), 2),
        "p99_ms": round(_percentile(values, 0.99), 2),
        "max_ms": round(values[-1], 2),
        "stutters_over_2x_median": sum(1 for v in values if v > 2 * median),
    }


# The time bb-run's GPU thread spent on one new pipeline (vk_pipeline_cache.cpp): translating its
# shaders and building it, or the part before a worker creates it ("Pipeline build"), and waiting
# for a worker ("Pipeline wait"). The start is CLOCK_MONOTONIC, the clock of frames.csv.
BUILD_LINE = re.compile(r"Pipeline (?:build|wait): \w+ 0x[0-9a-f]+ at (\d+) ns, (\d+) us")


@dataclass
class Build:
    start_ns: int
    duration_ns: int


def read_builds(log: str) -> list[Build]:
    return [Build(int(m[1]), int(m[2]) * 1000) for m in BUILD_LINE.finditer(log)]


def build_stalls(frames: list[Frame], builds: list[Build]) -> dict | None:
    """How the builds spread over the frame intervals, None without builds. A build counts against
    each interval it overlaps, by the overlap; the intervals a capture lengthened are left out, as
    in intervals_ms. The longest interval with a build in it is the stall the builds took part in."""
    if not builds:
        return None
    builds = sorted(builds, key=lambda b: b.start_ns)
    in_interval: list[tuple[float, float]] = []  # (interval ms, build ms within it)
    first = 0
    for previous, current in zip(frames, frames[1:]):
        if previous.captured or current.captured:
            continue
        while first < len(builds) and \
                builds[first].start_ns + builds[first].duration_ns <= previous.present_ns:
            first += 1
        overlap = 0
        for build in builds[first:]:
            if build.start_ns >= current.present_ns:
                break
            overlap += min(build.start_ns + build.duration_ns, current.present_ns) - \
                max(build.start_ns, previous.present_ns)
        if overlap > 0:
            in_interval.append(((current.present_ns - previous.present_ns) / 1e6, overlap / 1e6))
    return {
        "builds": len(builds),
        "total_ms": round(sum(b.duration_ns for b in builds) / 1e6, 1),
        "longest_build_ms": round(max(b.duration_ns for b in builds) / 1e6, 2),
        "intervals_with_builds": len(in_interval),
        "max_interval_with_builds_ms": round(max((i for i, _ in in_interval), default=0), 2),
        "max_build_ms_in_one_interval": round(max((b for _, b in in_interval), default=0), 2),
        "intervals_over_50ms_with_builds": sum(1 for i, _ in in_interval if i > 50),
    }


def segment_stats(frames: list[Frame], marks: list[tuple[int, str]]) -> dict[str, dict | None]:
    """Frame statistics per named segment, None for one with no presents. A mark (ns, name) starts
    a segment."""
    result = {}
    for (start, name), (end, _) in zip(marks, [*marks[1:], (1 << 63, "")]):
        result[name] = frame_stats([f for f in frames if start <= f.present_ns < end])
    return result


def segment_build_stalls(frames: list[Frame], builds: list[Build],
                         marks: list[tuple[int, str]]) -> dict[str, dict | None]:
    """build_stalls per named segment, by the start of each build, as segment_stats splits."""
    result = {}
    for (start, name), (end, _) in zip(marks, [*marks[1:], (1 << 63, "")]):
        result[name] = build_stalls([f for f in frames if start <= f.present_ns < end],
                                    [b for b in builds if start <= b.start_ns < end])
    return result
