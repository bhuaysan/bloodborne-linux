#!/usr/bin/env python3
"""Test rig: runs the reference or our program under limits and writes what happened to analysis/.

  rig.py setup                          download and check the reference (shadPS4 v.0.21.0)
  rig.py run reference new-game-to-clinic    a scenario from tools/rig/scenarios/ on the reference
  rig.py run bb-run --time 120          our program under the same limits, with no input
  rig.py run reference --user-from DIR ...   start with a copy of an earlier user/ directory
  rig.py compare REF_RUN REF_RUN OUR_RUN     our menu captures and time against two reference runs

Output of each run goes to analysis/rig/runs/<stamp>-<target>-<scenario>/: target.log, events.csv,
frames.csv, memory.csv, shots/*.png, kernel.log, summary.json. Exit status 0 means the scenario
finished (or a run without one reached its time limit) with no kernel GPU fault and an unchanged
game directory.

The game directory is mounted read-only for the target. Nothing this tool writes is ever committed:
analysis/ is excluded from git.
"""

from __future__ import annotations

import argparse
import csv
import datetime
import hashlib
import json
import os
import re
import resource
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import zipfile
from dataclasses import dataclass
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[0] / "common"))
sys.path.insert(0, str(HERE.parents[0] / "self2elf"))
import compare  # noqa: E402
import report  # noqa: E402
import scenario as scn  # noqa: E402
import self2elf  # noqa: E402
import toolenv  # noqa: E402
import vpad  # noqa: E402

REFERENCE_TAG = "v.0.21.0"
REFERENCE_ASSET = "shadps4-linux-sdl-0.21.0.zip"
REFERENCE_ZIP_SHA256 = "7c41c58254461d742b5edf564e23795697b63e9e46de11d0e1ed67bd2cbaae68"
REFERENCE_BIN_SHA256 = "1f9861192311392ee2fab0f48849819f6388650e768e1060aafbaded04370cd1"
LAYER_NAME = "VK_LAYER_BB_rig"
ANSI = re.compile(r"\x1b\[[0-9;]*m")
THUMB_SIZE = 64 * 36
# Every picture blocks one present of the target, so picture waits ask for at most one a second.
MIN_PICTURE_INTERVAL = 1.0
DEFAULT_TIME = 600
DEFAULT_MEMORY = "12G"
# SDL's message boxes go through zenity. On a fresh data directory shadPS4 asks whether to migrate
# saves ("Save Migration", src/core/user_manager.cpp) and waits for the answer. The stub says
# "Do nothing" to any question, and logs it, so no run ever blocks on a dialog.
ZENITY_STUB = """#!/bin/sh
printf '%s\\n' "$*" >> "$BB_RIG_DIR/zenity-calls.txt"
if [ "$1" = "--version" ]; then echo 4.0.0; exit 0; fi
echo "Do nothing"
exit 1
"""


def rig_dir() -> Path:
    return toolenv.analysis_dir() / "rig"


def reference_root() -> Path:
    return rig_dir() / "reference" / REFERENCE_TAG


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def setup_reference(root: Path | None = None) -> Path:
    """Downloads and unpacks the pinned release into root and checks both hashes."""
    root = root or reference_root()
    binary = root / "squashfs-root" / "usr" / "bin" / "shadps4"
    if binary.is_file() and sha256(binary) == REFERENCE_BIN_SHA256:
        return root
    download = root.parent / "download"
    shutil.rmtree(download, ignore_errors=True)
    download.mkdir(parents=True)
    subprocess.run(["gh", "release", "download", REFERENCE_TAG, "-R", "shadps4-emu/shadPS4",
                    "-p", REFERENCE_ASSET, "--dir", str(download)], check=True)
    archive = download / REFERENCE_ASSET
    if sha256(archive) != REFERENCE_ZIP_SHA256:
        raise SystemExit(f"{REFERENCE_ASSET}: unexpected SHA-256")
    extracted = download / "extracted"
    with zipfile.ZipFile(archive) as z:
        z.extractall(extracted)
    appimage = next(extracted.rglob("*.AppImage"))
    appimage.chmod(0o755)
    root.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(appimage), "--appimage-extract"], cwd=root, check=True,
                   stdout=subprocess.DEVNULL)
    if sha256(binary) != REFERENCE_BIN_SHA256:
        raise SystemExit("reference binary: unexpected SHA-256")
    shutil.rmtree(download)
    return root


def build_layer(destination: Path | None = None) -> Path:
    """Compiles the Vulkan layer if its source is newer than the binary. Returns its directory."""
    destination = destination or rig_dir() / "layer"
    source = HERE / "layer" / "bb_rig_layer.cpp"
    binary = destination / "libVkLayer_bb_rig.so"
    if not binary.exists() or binary.stat().st_mtime < source.stat().st_mtime:
        destination.mkdir(parents=True, exist_ok=True)
        subprocess.run(["g++", "-std=c++20", "-O2", "-g", "-Wall", "-Wextra",
                        "-Wno-missing-field-initializers", "-Werror", "-fPIC", "-shared",
                        "-fvisibility=hidden", "-o", str(binary), str(source)], check=True)
    shutil.copy(HERE / "layer" / "VkLayer_bb_rig.json", destination)
    return destination


def wrap_command(argv: list[str], game_dir: Path, unit: str, seconds: int, memory: str) -> list[str]:
    """The target's command under memory limit, time limit and read-only game directory."""
    return ["systemd-run", "--user", "--scope", "-q", f"--unit={unit}",
            "-p", f"MemoryMax={memory}", "-p", "MemorySwapMax=0", "-p", "TimeoutStopSec=15", "--",
            *read_only_game_prefix(game_dir),
            "timeout", "-k", "10", str(seconds), *argv]


def read_only_game_prefix(game_dir: Path) -> list[str]:
    return ["bwrap", "--dev-bind", "/", "/", "--ro-bind", str(game_dir), str(game_dir), "--"]


def check_game_read_only(game_dir: Path) -> None:
    """Refuses to run when the sandbox would not make the game directory read-only."""
    # `test` only asks the kernel, so the probe can't write into the game directory. It exits 0
    # when the directory is not writable, and any other status (bwrap failing too) refuses.
    probe = subprocess.run([*read_only_game_prefix(game_dir), "sh", "-c", 'test ! -w "$1"', "_",
                            str(game_dir)])
    if probe.returncode != 0:
        raise SystemExit("the sandbox does not make the game directory read-only")


@dataclass
class Program:
    """How to start one target. `work` is its working directory, inside the run directory."""
    argv: list[str]
    env: dict[str, str]
    cwd: Path


def reference_program(run: Path, game_dir: Path, user_from: Path | None) -> Program:
    root = reference_root()
    binary = root / "squashfs-root" / "usr" / "bin" / "shadps4"
    if not binary.is_file() or sha256(binary) != REFERENCE_BIN_SHA256:
        raise SystemExit("reference missing or changed, run `rig.py setup`")
    work = run / "work"
    work.mkdir()
    if user_from:
        shutil.copytree(user_from, work / "user")
    else:
        (work / "user").mkdir()  # its presence makes the emulator use this portable directory
    stubs = run / "stubs"
    stubs.mkdir()
    (stubs / "zenity").write_text(ZENITY_STUB)
    (stubs / "zenity").chmod(0o755)
    env = {"PATH": f"{stubs}:{os.environ['PATH']}", "SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS": "1"}
    return Program([str(root / "squashfs-root" / "AppRun"), "-f", "false",
                    "-g", str(game_dir / "eboot.bin")], env, work)


def find_bb_run(preset: str) -> Path:
    for checkout in (toolenv.REPO_ROOT, toolenv.main_checkout()):
        candidate = checkout / "build" / preset / "src" / "tools" / "bb-run"
        if candidate.is_file():
            return candidate
    raise SystemExit(f"no bb-run in build/{preset}, build the preset first")


def bb_run_program(run: Path, game_dir: Path, preset: str, window: bool, work_root: Path) -> Program:
    # bb-run refuses runtime directories inside the source checkout, so its XDG roots are outside
    # analysis/. They are per run, so runs never share or inherit save data.
    env = {f"XDG_{kind}_HOME": str(work_root / kind.lower())
           for kind in ("DATA", "CACHE", "CONFIG", "STATE")}
    argv = [str(find_bb_run(preset)), "--game-dir", str(game_dir), "--profile", "rig",
            "--no-log-file"]
    if not window:
        argv.append("--headless")
    return Program(argv, env, run)


def loaded_scenario(name: str | None) -> tuple[scn.Scenario, str]:
    if not name:
        return scn.Scenario(), "none"
    path = Path(name)
    if not path.exists():
        path = HERE / "scenarios" / f"{name}.scn"
    if not path.is_file():
        raise SystemExit(f"no scenario named {name}")
    return scn.parse(path.read_text()), path.stem


class RunningTarget:
    """A started target as the scenario interpreter sees it."""

    def __init__(self, run: Path, process: subprocess.Popen, pad: vpad.VirtualPad | None):
        self.run_dir = run
        self.process = process
        self.pad = pad
        self._log = open(run / "target.log", "rb")
        self._ctl = os.open(run / "ctl", os.O_RDWR | os.O_NONBLOCK)
        self._seq = b""
        self._pending = b""
        self._pending_since = 0.0
        self._last_picture: bytes | None = None
        self._last_picture_at = 0.0
        self._frames_size = 0
        self._frames = 0

    def alive(self) -> bool:
        return self.process.poll() is None

    def log_tail(self) -> str:
        return ANSI.sub("", self._log.read().decode("utf-8", "replace"))

    def frame_count(self) -> int:
        path = self.run_dir / "frames.csv"
        try:
            size = path.stat().st_size
        except FileNotFoundError:
            return 0
        if size != self._frames_size:
            with path.open("rb") as f:
                f.seek(self._frames_size)
                self._frames += f.read().count(b"\n")
            self._frames_size = size
        return max(0, self._frames - 1)  # the header line

    def _command(self, text: str) -> None:
        os.write(self._ctl, text.encode() + b"\n")

    def thumb(self) -> bytes | None:
        """Requests a fresh picture and waits briefly for one. None when none arrived."""
        now = time.monotonic()
        if self._last_picture is not None and now - self._last_picture_at < MIN_PICTURE_INTERVAL:
            return self._last_picture
        if not self._pending_since or now - self._pending_since > 2:
            self._command("thumb")
            self._pending_since = now
        deadline = now + 1.0
        path = self.run_dir / "thumb.bin"
        while time.monotonic() < deadline:
            try:
                data = path.read_bytes()
            except FileNotFoundError:
                data = b""
            end = data.find(b"\n")
            if end > 0 and data[:end] != self._seq and len(data) == end + 1 + THUMB_SIZE:
                self._seq = data[:end]
                self._pending_since = 0.0
                self._last_picture = data[end + 1:]
                self._last_picture_at = time.monotonic()
                return self._last_picture
            time.sleep(0.02)
            if not self.alive():
                return None
        return None

    def shot(self, name: str) -> None:
        self._command(f"shot {name}")
        time.sleep(0.2)  # the next present takes it, and the raw file is converted after the run


class Events:
    """events.csv: what the rig did and when. log_bytes ties each step to a place in target.log."""

    def __init__(self, path: Path, log: Path):
        self._file = open(path, "w", buffering=1, newline="")
        self._csv = csv.writer(self._file)
        self._csv.writerow(["mono_ns", "utc", "log_bytes", "kind", "text"])
        self._log = log
        self.marks: list[tuple[int, str]] = []

    def __call__(self, kind: str, text: str) -> None:
        now = time.monotonic_ns()
        stamp = datetime.datetime.now(datetime.UTC).strftime("%H:%M:%S.%f")[:-3]
        size = self._log.stat().st_size if self._log.exists() else 0
        self._csv.writerow([now, stamp, size, kind, text])
        if kind == "mark":
            self.marks.append((now, text))
        print(f"[{stamp}] {kind}: {text}", flush=True)


class MemorySampler(threading.Thread):
    """Samples the target's cgroup once a second into memory.csv and keeps the peak."""

    def __init__(self, unit: str, path: Path):
        super().__init__(daemon=True)
        self.unit, self.path = unit, path
        self.peak = 0
        self.peak_program = 0
        self.oom_kills = 0
        self.stopping = threading.Event()

    def _cgroup(self) -> Path | None:
        run = subprocess.run(["systemctl", "--user", "show", "-p", "ControlGroup", "--value",
                              f"{self.unit}.scope"], capture_output=True, text=True)
        name = run.stdout.strip()
        return Path("/sys/fs/cgroup" + name) if name else None

    def run(self) -> None:
        with self.path.open("w", buffering=1) as out:
            out.write("mono_ns,current,peak,program\n")
            cgroup = None
            while not self.stopping.wait(1.0):
                cgroup = cgroup or self._cgroup()
                if cgroup is None:
                    continue
                try:
                    current = int((cgroup / "memory.current").read_text())
                    self.peak = max(self.peak, int((cgroup / "memory.peak").read_text()))
                    # The cgroup also counts the page cache of the game's files. Anonymous and
                    # shared memory is what the program itself uses; the guest's memory is shmem.
                    stats = dict(line.split() for line in
                                 (cgroup / "memory.stat").read_text().splitlines())
                    program = int(stats.get("anon", 0)) + int(stats.get("shmem", 0))
                    self.peak_program = max(self.peak_program, program)
                    events = dict(line.split() for line in
                                  (cgroup / "memory.events").read_text().splitlines())
                    self.oom_kills = max(self.oom_kills, int(events.get("oom_kill", 0)))
                except (OSError, ValueError):
                    cgroup = None  # the scope has ended, or is not there yet
                    continue
                out.write(f"{time.monotonic_ns()},{current},{self.peak},{program}\n")


def stop_target(unit: str, process: subprocess.Popen) -> None:
    subprocess.run(["systemctl", "--user", "stop", f"{unit}.scope"], capture_output=True)
    try:
        process.wait(timeout=30)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def kernel_journal(since: float) -> str | None:
    """The kernel log since a time, or None when it could not be read."""
    run = subprocess.run(["journalctl", "-k", "--since", f"@{int(since)}", "--no-pager"],
                         capture_output=True, text=True)
    return run.stdout if run.returncode == 0 or "-- No entries --" in run.stdout else None


def convert_shots(run: Path) -> list[str]:
    errors = []
    for raw in sorted((run / "shots").glob("*.raw")):
        try:
            report.raw_to_png(raw, raw.with_suffix(".png"))
            raw.unlink()
        except (OSError, ValueError) as e:
            errors.append(f"{raw.name}: {e}")
    return errors


def run_target(args: argparse.Namespace) -> int:
    game_dir = toolenv.load_game_dir()
    if game_dir is None:
        raise SystemExit("BB_GAME_DIR is not set (environment or local.env)")
    toolenv.read_pinned(game_dir / "eboot.bin", self2elf.KNOWN_FILES)
    scenario, scenario_name = loaded_scenario(args.scenario)
    if args.no_layer and scenario.uses_frames():
        raise SystemExit("this scenario uses pictures, which need the layer")
    seconds = args.time or scenario.time_limit or DEFAULT_TIME
    memory = args.memory or scenario.memory_limit or DEFAULT_MEMORY
    check_game_read_only(game_dir)

    stamp = datetime.datetime.now(datetime.UTC).strftime("%Y%m%d-%H%M%S")
    run = rig_dir() / "runs" / f"{stamp}-{args.target}-{scenario_name}"
    (run / "shots").mkdir(parents=True)
    os.mkfifo(run / "ctl")

    layer = build_layer()
    work_root = None
    if args.target == "reference":
        program = reference_program(run, game_dir, args.user_from)
    else:
        work_root = Path(tempfile.mkdtemp(prefix="bb-rig-",
                                          dir=os.environ.get("XDG_CACHE_HOME") or Path.home() / ".cache"))
        program = bb_run_program(run, game_dir, args.preset, args.window, work_root)

    env = {**os.environ, **program.env, "BB_RIG_DIR": str(run)}
    if not args.no_layer:
        env |= {"VK_LAYER_PATH": str(layer), "VK_INSTANCE_LAYERS": LAYER_NAME}
    unit = f"bb-rig-{stamp}"
    command = wrap_command(program.argv, game_dir, unit, seconds, memory)
    pad = vpad.VirtualPad() if scenario.uses_pad() else None
    before = report.tree_state(game_dir)
    started = time.time()
    events = Events(run / "events.csv", run / "target.log")
    events("start", f"{args.target} time={seconds}s memory={memory}")
    with open(run / "target.log", "wb") as log:
        # A core dump of a run takes a minute and many gigabytes, so a crash doesn't write one.
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, cwd=program.cwd,
                                   env=env, stdin=subprocess.DEVNULL,
                                   preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
    memory_sampler = MemorySampler(unit, run / "memory.csv")
    memory_sampler.start()
    target = RunningTarget(run, process, pad)
    failure = None
    try:
        scn.Interpreter(target, events).run(scenario)
        if args.hold:
            events("hold", f"{args.hold}s")
            end = time.monotonic() + args.hold
            while process.poll() is None and time.monotonic() < end:
                time.sleep(0.5)
        if not scenario.steps:
            process.wait(timeout=seconds + 40)
        elif process.poll() is not None:
            # Waits notice a dead target, but the last steps and --hold may not.
            failure = "the target exited before the end of the scenario"
            events("fail", failure)
    except scn.StepFailed as e:
        failure = str(e)
        events("fail", failure)
        if process.poll() is None:
            target.shot("failure")
            time.sleep(1)
    except KeyboardInterrupt:
        failure = "interrupted"
    finally:
        if process.poll() is None:
            stop_target(unit, process)
        if pad:
            pad.close()
        memory_sampler.stopping.set()
        memory_sampler.join(timeout=3)

    status = process.returncode
    frames = report.read_frames(run / "frames.csv")
    conversion_errors = convert_shots(run)
    journal = kernel_journal(started)
    (run / "kernel.log").write_text(journal or "")
    faults = report.kernel_faults(journal or "")
    if journal is None:
        failure = failure or "could not read the kernel log"
    changes = report.tree_changes(before, report.tree_state(game_dir))
    if work_root:
        if (work_root / "state").exists():
            shutil.copytree(work_root / "state", run / "target-state", dirs_exist_ok=True)
        shutil.rmtree(work_root, ignore_errors=True)
    zenity = run / "zenity-calls.txt"
    if not scenario.steps and status not in (0, 124):
        failure = f"exit status {status}"
    if memory_sampler.oom_kills:
        failure = failure or "out of memory"

    summary = {
        "target": args.target, "scenario": scenario_name, "run": run.name,
        "started_utc": datetime.datetime.fromtimestamp(started, datetime.UTC).isoformat(),
        "duration_s": round(time.time() - started, 1),
        "time_limit_s": seconds, "memory_limit": memory, "exit_status": status,
        "scenario_ok": failure is None, "failure": failure,
        "peak_memory_bytes": memory_sampler.peak, "peak_program_bytes": memory_sampler.peak_program,
        "oom_kills": memory_sampler.oom_kills,
        "frames": report.frame_stats(frames),
        "segments": report.segment_stats(frames, events.marks),
        "kernel_gpu_faults": faults, "game_dir_changes": changes,
        "zenity_called": zenity.exists(), "shot_errors": conversion_errors,
    }
    (run / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))
    print(f"\noutput: {run}")
    return 0 if failure is None and not faults and not changes else 1


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("setup", help="download and check the reference")
    run = commands.add_parser("run", help="run a target")
    run.add_argument("target", choices=["reference", "bb-run"])
    run.add_argument("scenario", nargs="?", help="name in tools/rig/scenarios or a path")
    run.add_argument("--time", type=int, help="time limit in seconds")
    run.add_argument("--memory", help="memory limit, as systemd takes it (12G)")
    run.add_argument("--hold", type=float, default=0, help="seconds to keep running after the end")
    run.add_argument("--user-from", type=Path, help="copy this user/ directory (reference)")
    run.add_argument("--preset", default="relwithdebinfo", help="bb-run build preset")
    run.add_argument("--window", action="store_true", help="let bb-run open its window")
    run.add_argument("--no-layer", action="store_true",
                     help="without the Vulkan layer, so no frame times and no picture steps")
    compared = commands.add_parser("compare", help="compare menu captures with the reference")
    compared.add_argument("runs", nargs=3, type=Path, metavar="RUN",
                          help="two reference run directories, then ours")
    compared.add_argument("--shots", default=compare.DEFAULT_SHOTS,
                          help="which shots of each run to compare (a glob)")
    args = parser.parse_args(argv)
    if args.command == "compare":
        return compare.run_compare(*args.runs, args.shots)
    if args.command == "setup":
        print(setup_reference())
        return 0
    return run_target(args)


if __name__ == "__main__":
    sys.exit(main())
