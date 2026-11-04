#!/usr/bin/env python3
"""Sync our Ghidra annotations with symbols/functions.csv.

export  reads the function names and signatures we set in a Ghidra project and merges them into
        functions.csv. The status and file columns are ours and stay as they are.
import  applies functions.csv to a project, e.g. a fresh one rebuilt from your own eboot.

Only user-defined names and signatures go out: Ghidra's own names (FUN_..., thunks, symbols from
the ELF) are left in the project. Ghidra runs headless through PyGhidra, which ships with Ghidra
and needs a one-time setup. See symbols/README.md, section "Sync".

Usage:
    ghidrasync.py export PROJECT.gpr [--program NAME] [--symbols DIR]
    ghidrasync.py import PROJECT.gpr [--program NAME | --elf FILE] [--symbols DIR]

--elf imports FILE into the project and analyzes it before applying the symbols. On the eboot
that takes about an hour and 11 GiB, so run it under the analysis lock (see symbols/README.md).
Without --elf the program is opened as it is, with no analysis.
"""

from __future__ import annotations

import argparse
import csv
import importlib.util
import io
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "common"))
from toolenv import REPO_ROOT, local_env  # noqa: E402

COLUMNS = ["address", "name", "signature", "status", "file"]
SCRIPTS = Path(__file__).resolve().parent / "ghidra_scripts"
DEFAULT_SYMBOLS = REPO_ROOT / "symbols"
DEFAULT_PROGRAM = "eboot.elf"


class SyncError(Exception):
    pass


# --- functions.csv (pure Python, no Ghidra) ---------------------------------------------------

def read_functions(path: Path) -> dict[int, dict[str, str]]:
    """Rows of functions.csv keyed by address. A missing file is an empty table."""
    if not path.is_file():
        return {}
    with path.open(newline="") as src:
        reader = csv.DictReader(src)
        if reader.fieldnames != COLUMNS:
            raise SyncError(f"{path}: expected columns {','.join(COLUMNS)}")
        rows = {}
        for line, row in enumerate(reader, start=2):
            try:
                address = int(row["address"], 16)
            except ValueError:
                raise SyncError(f"{path}:{line}: bad address {row['address']!r}") from None
            if address in rows:
                raise SyncError(f"{path}:{line}: duplicate address {row['address']}")
            rows[address] = {key: row[key] or "" for key in COLUMNS[1:]}
        return rows


def format_functions(rows: dict[int, dict[str, str]]) -> str:
    """functions.csv text: sorted by address, fixed address width, \\n line ends."""
    out = io.StringIO()
    writer = csv.writer(out, lineterminator="\n")
    writer.writerow(COLUMNS)
    for address in sorted(rows):
        writer.writerow([f"0x{address:08x}", *(rows[address][key] for key in COLUMNS[1:])])
    return out.getvalue()


def merge_functions(rows: dict[int, dict[str, str]],
                    exported: list[dict]) -> dict[int, dict[str, str]]:
    """Merge Ghidra's export into the CSV rows.

    Ghidra sets name and signature when it has a value; an empty value keeps the CSV's, so a
    project that lacks some annotations never erases them. Rows Ghidra does not know stay.
    status and file are never touched; new rows start as `named` (or `unknown` without a name).
    """
    merged = {address: dict(row) for address, row in rows.items()}
    for item in exported:
        row = merged.setdefault(item["address"], {
            "name": "", "signature": "", "status": "named" if item["name"] else "unknown",
            "file": ""})
        for key in ("name", "signature"):
            if item[key]:
                row[key] = item[key]
    return merged


def import_rows(rows: dict[int, dict[str, str]]) -> list[dict]:
    """What the Ghidra import script applies: rows with a name or a signature."""
    return [{"address": address, "name": row["name"], "signature": row["signature"]}
            for address, row in sorted(rows.items()) if row["name"] or row["signature"]]


# --- headless Ghidra ---------------------------------------------------------------------------

def ghidra_dir() -> Path:
    value = local_env("GHIDRA_INSTALL_DIR")
    if not value:
        raise SyncError("GHIDRA_INSTALL_DIR is not set (environment or local.env)")
    install = Path(value).expanduser()
    if not (install / "Ghidra" / "application.properties").is_file():
        raise SyncError(f"GHIDRA_INSTALL_DIR is not a Ghidra install: {install}")
    return install


def ghidra_venv(install: Path) -> Path:
    """The virtual environment pyghidraRun creates in Ghidra's user settings directory."""
    props = dict(line.split("=", 1) for line in
                 (install / "Ghidra" / "application.properties").read_text().splitlines()
                 if "=" in line and not line.startswith(("#", "!")))
    name = f"ghidra_{props['application.version']}_{props['application.release.name']}"
    config = os.environ.get("XDG_CONFIG_HOME") or str(Path.home() / ".config")
    return Path(config) / "ghidra" / name / "venv"


def pyghidra_python(install: Path) -> str:
    """A Python interpreter that has pyghidra: this one, else Ghidra's own venv.

    pyghidraRun is not used: it asks on stdin before installing PyGhidra.
    """
    if importlib.util.find_spec("pyghidra"):
        return sys.executable
    python = ghidra_venv(install) / "bin" / "python3"
    if python.is_file() and subprocess.run([str(python), "-c", "import pyghidra"],
                                           capture_output=True).returncode == 0:
        return str(python)
    raise SyncError(f"PyGhidra is not set up. Run {install / 'support' / 'pyghidraRun'} once "
                    "and let it install PyGhidra into a new Ghidra virtual environment "
                    "(see symbols/README.md, section \"Sync\")")


def headless_command(python: str, install: Path, project: Path, target: list[str],
                     scripts: list[list[str]]) -> list[str]:
    """analyzeHeadless through PyGhidra, so the scripts in ghidra_scripts/ run as Python 3."""
    cmd = [python, "-m", "pyghidra.ghidra_launch", "--install-dir", str(install),
           "ghidra.app.util.headless.AnalyzeHeadless", str(project.parent), project.stem,
           *target, "-scriptPath", str(SCRIPTS)]
    for script in scripts:
        cmd += ["-postScript", *script]
    return cmd


def run_headless(project: Path, target: list[str], scripts: list[list[str]]) -> str:
    """Runs Ghidra and returns the tail of its output."""
    install = ghidra_dir()
    cmd = headless_command(pyghidra_python(install), install, project, target, scripts)
    project.parent.mkdir(parents=True, exist_ok=True)
    run = subprocess.run(cmd, capture_output=True, text=True, stdin=subprocess.DEVNULL)
    tail = "\n".join((run.stdout + run.stderr).splitlines()[-40:])
    if run.returncode != 0:
        raise SyncError(f"Ghidra exited with {run.returncode}:\n{tail}")
    return tail


def run_scripts(project: Path, target: list[str], tmp: Path,
                to_ghidra: list[dict] | None, export: bool) -> list[dict] | None:
    """Run import and/or export in one headless session. Returns the exported rows."""
    scripts = []
    done = tmp / "done.json"
    if to_ghidra is not None:
        (tmp / "in.json").write_text(json.dumps({"functions": to_ghidra}))
        scripts.append(["SymbolsImport.py", str(tmp / "in.json"), str(done)])
    out = tmp / "out.json"
    if export:
        scripts.append(["SymbolsExport.py", str(out)])
    tail = run_headless(project, target, scripts)
    # analyzeHeadless exits 0 even when a script fails, so each script writes its result last.
    if to_ghidra is not None and not done.is_file():
        raise SyncError(f"the import script did not finish:\n{tail}")
    if export and not out.is_file():
        raise SyncError(f"the export script did not finish:\n{tail}")
    return json.loads(out.read_text())["functions"] if export else None


def sync(project: Path, symbols: Path, target: list[str], do_import: bool,
         do_export: bool) -> None:
    csv_path = symbols / "functions.csv"
    rows = read_functions(csv_path)
    with tempfile.TemporaryDirectory(prefix="ghidrasync-") as tmp:
        exported = run_scripts(project, target, Path(tmp),
                               import_rows(rows) if do_import else None, do_export)
    if exported is not None:
        text = format_functions(merge_functions(rows, exported))
        if not csv_path.is_file() or csv_path.read_text() != text:
            csv_path.parent.mkdir(parents=True, exist_ok=True)
            csv_path.write_text(text)
        print(f"exported {len(exported)} functions to {csv_path}")
    if do_import:
        print(f"imported {len(import_rows(rows))} functions into {project}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("command", choices=["export", "import"])
    parser.add_argument("project", type=Path, help="Ghidra project file (.gpr)")
    parser.add_argument("--program", default=DEFAULT_PROGRAM,
                        help=f"program in the project (default: {DEFAULT_PROGRAM})")
    parser.add_argument("--elf", type=Path, help="import: add and analyze this ELF first")
    parser.add_argument("--symbols", type=Path, default=DEFAULT_SYMBOLS,
                        help="symbols directory (default: symbols/ of this checkout)")
    args = parser.parse_args(argv)
    if args.project.suffix != ".gpr":
        parser.error("PROJECT must be a .gpr file")
    if args.elf and args.command != "import":
        parser.error("--elf works with import only")
    if args.elf:
        target = ["-import", str(args.elf.resolve())]
    else:
        target = ["-process", args.program, "-noanalysis"]
        if args.command == "export":
            target.append("-readOnly")
    try:
        sync(args.project.resolve(), args.symbols, target, args.command == "import",
             args.command == "export")
    except SyncError as err:
        print(f"ghidrasync: {err}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
