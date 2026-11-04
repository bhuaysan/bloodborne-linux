#!/usr/bin/env python3
"""Sync our Ghidra annotations with symbols/functions.csv and symbols/types/.

export  reads the function names and signatures we set in a Ghidra project and merges them into
        functions.csv. The status and file columns are ours and stay as they are.
        Data types in /bb/<subsystem> go to types/<subsystem>.h.
import  applies functions.csv and types/ to a project, e.g. a fresh one rebuilt from your eboot.

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
import re
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
HEADER_NOTE = "/* Ghidra category /bb/{}: written by tools/ghidra-sync, see symbols/README.md */"
SUBSYSTEM = re.compile(r"[A-Za-z0-9_]+")
INCLUDE = re.compile(r'^#include "(\w+)\.h"$', re.M)
SIZE_ASSERT = re.compile(r'^_Static_assert\(sizeof\((?:struct|union|enum) (\w+)\) == '
                         r'(0x[0-9a-fA-F]+|\d+), "\w+"\);$', re.M)
ENUM = re.compile(r"^enum (\w+) \{\n((?:    \w+ = -?\d+,\n)*)\};$", re.M)
ENUM_VALUE = re.compile(r"(\w+) = (-?\d+)")
IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
C_KEYWORDS = frozenset("""
    auto break case char const continue default do double else enum extern float for goto if
    inline int long register restrict return short signed sizeof static struct switch typedef
    union unsigned void volatile while _Alignas _Alignof _Atomic _Bool _Complex _Generic
    _Imaginary _Noreturn _Static_assert _Thread_local
""".split())


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


# --- types/*.h (pure Python, no Ghidra) -------------------------------------------------------
# TypesExport.py describes each type as JSON. A reference to a type is {"name"} for a builtin,
# {"name", "tag", "subsystem"} for one of ours, {"pointer": ref or None} or {"array", "count"}.

def topo_order(deps: dict[str, set[str]], what: str) -> list[str]:
    """Names with their dependencies first, otherwise sorted by name."""
    done: list[str] = []
    while len(done) < len(deps):
        ready = [n for n in sorted(deps) if n not in done and deps[n] <= set(done)]
        if not ready:
            raise SyncError(f"{what} depend on each other: "
                            + ", ".join(sorted(set(deps) - set(done))))
        done.append(ready[0])
    return done


def declare(ref: dict, inner: str = "") -> str:
    """The C declaration of `inner` with type `ref`."""
    if "pointer" in ref:
        return declare(ref["pointer"] or {"name": "void"}, "*" + inner)
    if "array" in ref:
        return declare(ref["array"], f"{inner}[{ref['count']}]")
    base = f"{ref['tag']} {ref['name']}" if ref.get("tag") else ref["name"]
    return f"{base} {inner}" if inner else base


def type_refs(ref: dict, by_value: bool = True) -> list[tuple[str, str, bool]]:
    """(subsystem, name, by value) of our types that `ref` uses."""
    if "pointer" in ref:
        return type_refs(ref["pointer"], False) if ref["pointer"] else []
    if "array" in ref:
        return type_refs(ref["array"], by_value)
    return [(ref["subsystem"], ref["name"], by_value)] if "subsystem" in ref else []


def uses(item: dict) -> list[tuple[str, str, bool]]:
    if item["kind"] == "typedef":  # naming a struct or union does not need its body
        return type_refs(item["type"], "subsystem" not in item["type"])
    return [use for f in item.get("fields", []) for use in type_refs(f["type"])]


def format_fields(item: dict) -> list[str]:
    """Struct or union members with their offsets. Gaps in a struct become _pad_ arrays."""
    lines, end = [], 0
    for field in sorted(item["fields"], key=lambda f: f["offset"]):
        if item["kind"] == "struct" and field["offset"] > end:
            lines.append(f"    /* 0x{end:04x} */ undefined1 _pad_0x{end:04x}"
                         f"[{field['offset'] - end}];")
        elif item["kind"] == "struct" and field["offset"] < end:
            raise SyncError(f"{item['name']}: field {field['name']} overlaps the one before it")
        lines.append(f"    /* 0x{field['offset']:04x} */ "
                     f"{declare(field['type'], field['name'])};")
        end = max(end, field["offset"] + field["size"])
    if item["kind"] == "union" and item["size"] > end:
        raise SyncError(f"{item['name']}: no member covers all {item['size']} union bytes")
    if item["kind"] == "struct" and item["size"] > end:
        lines.append(f"    /* 0x{end:04x} */ undefined1 _pad_0x{end:04x}[{item['size'] - end}];")
    return lines


def format_type(item: dict) -> list[str]:
    kind, name = item["kind"], item["name"]
    if kind == "typedef":
        return [f"typedef {declare(item['type'], name)};"]
    if kind == "enum":
        values = sorted(item["values"], key=lambda value: (value[1], value[0]))
        body = [f"    {n} = {v}," for n, v in values]
    else:
        body = format_fields(item)
    return [f"{kind} {name} {{", *body, "};",
            f'_Static_assert(sizeof({kind} {name}) == {item["size"]:#x}, "{name}");']


def format_header(subsystem: str, items: list[dict]) -> str:
    """types/<subsystem>.h: includes, forward declarations, then the types in dependency order.
    Every gap is spelled out and packing is 1, so the layout does not depend on alignment."""
    by_name = {item["name"]: item for item in items}

    def needs(s: str, n: str, by_value: bool) -> set[str]:
        """Types here that must precede a use of n; pointed-to structs are forward declared."""
        item = by_name.get(n) if s == subsystem else None
        if item is None or not by_value and item["kind"] in ("struct", "union"):
            return set()
        chain = type_refs(item["type"]) if by_value and item["kind"] == "typedef" else []
        return {n}.union(*(needs(*use) for use in chain))

    deps = {name: set().union(*(needs(*use) for use in uses(item))) - {name}
            for name, item in by_name.items()}
    others = sorted({s for item in items for s, _, _ in uses(item)} - {subsystem})
    lines = [HEADER_NOTE.format(subsystem), *(f'#include "{s}.h"' for s in others), "",
             "#pragma pack(push, 1)", ""]
    lines += [f"{item['kind']} {name};" for name, item in sorted(by_name.items())
              if item["kind"] in ("struct", "union")]
    for name in topo_order(deps, f"types in /bb/{subsystem}"):
        lines += ["", *format_type(by_name[name])]
    return "\n".join([*lines, "", "#pragma pack(pop)", ""])


def check_name(where: str, name: str) -> None:
    """Names go into the headers as they are, so they must be C identifiers the parser reads."""
    if not IDENTIFIER.fullmatch(name):
        raise SyncError(f"{where}: {name!r} is not a C identifier "
                        "(letters, digits and _, not starting with a digit)")
    if name in C_KEYWORDS:
        raise SyncError(f"{where}: {name} is a C keyword")
    if name.startswith("__"):
        raise SyncError(f"{where}: {name}: names starting with __ are reserved for the compiler")


def check_names(subsystem: str, items: list[dict]) -> None:
    """Type, field and enumerator names of one subsystem."""
    for item in items:
        where = f"/bb/{subsystem}/{item['name']}"
        check_name(f"/bb/{subsystem}: type name", item["name"])
        for field in item.get("fields", []):
            check_name(f"{where}: field name", field["name"])
        for value_name, _ in item.get("values", []):
            check_name(f"{where}: enum value name", value_name)


def write_types(types_dir: Path, exported: dict[str, list[dict]]) -> None:
    """Write each header that changed, or none if one fails. Other headers stay as they are."""
    for subsystem in exported:
        if not SUBSYSTEM.fullmatch(subsystem):
            raise SyncError(f"/bb/{subsystem}: use letters, digits and _ for subsystem names")
    for subsystem, items in sorted(exported.items()):
        check_names(subsystem, items)
    homes: dict[str, list[str]] = {}
    for subsystem, items in sorted(exported.items()):
        for item in items:
            homes.setdefault(item["name"], []).append(subsystem)
    clashes = [f"{n} (/bb/{', /bb/'.join(s)})" for n, s in sorted(homes.items()) if len(s) > 1]
    if clashes:  # C has one namespace for all headers
        raise SyncError("type names used in more than one subsystem: " + ", ".join(clashes))
    texts = {s: format_header(s, items) for s, items in exported.items()}
    topo_order({s: set(INCLUDE.findall(text)) & set(texts) for s, text in texts.items()},
               "subsystems in /bb (pointers count)")
    for subsystem, text in sorted(texts.items()):
        write_if_changed(types_dir / f"{subsystem}.h", text)


def read_headers(types_dir: Path) -> list[dict]:
    """The headers in include order, as TypesImport.py takes them. Its parser does not know
    #include (they become empty lines), makes every enum 4 bytes with 32-bit values and does not
    check _Static_assert, so the sizes and the enum values are read here."""
    texts = {path.stem: path.read_text() for path in sorted(types_dir.glob("*.h"))}
    deps = {name: set(INCLUDE.findall(text)) - {name} for name, text in texts.items()}
    for name, includes in deps.items():
        if includes - set(texts):
            raise SyncError(f"types/{name}.h includes a missing header: "
                            + ", ".join(sorted(includes - set(texts))))
    return [{"subsystem": name, "text": INCLUDE.sub("", texts[name]),
             "sizes": {t: int(size, 0) for t, size in SIZE_ASSERT.findall(texts[name])},
             "enums": {e: [[n, int(v)] for n, v in ENUM_VALUE.findall(body)]
                       for e, body in ENUM.findall(texts[name])}}
            for name in topo_order(deps, "headers in types/")]


def write_if_changed(path: Path, text: str) -> None:
    if not path.is_file() or path.read_text() != text:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)


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
                to_ghidra: dict | None, export: bool) -> dict | None:
    """Run import (to_ghidra: "headers", "functions") and/or export in one headless session.
    Types go first: signatures may use them. Returns what the export scripts wrote."""
    scripts, results = [], []
    if to_ghidra is not None:
        for script, key in (("TypesImport.py", "headers"), ("SymbolsImport.py", "functions")):
            src, done = tmp / f"{key}-in.json", tmp / f"{key}-done.json"
            src.write_text(json.dumps({key: to_ghidra[key]}))
            scripts.append([script, str(src), str(done)])
            results.append((script, done))
    if export:
        for script, key in (("SymbolsExport.py", "functions"), ("TypesExport.py", "types")):
            out = tmp / f"{key}-out.json"
            scripts.append([script, str(out)])
            results.append((script, out))
    tail = run_headless(project, target, scripts)
    # analyzeHeadless exits 0 even when a script fails, so each script writes its result last.
    for script, path in results:
        if not path.is_file():
            raise SyncError(f"{script} did not finish:\n{tail}")
    if not export:
        return None
    return {key: value for _, path in results[-2:]
            for key, value in json.loads(path.read_text()).items()}


def sync(project: Path, symbols: Path, target: list[str], do_import: bool,
         do_export: bool) -> None:
    csv_path = symbols / "functions.csv"
    rows = read_functions(csv_path)
    headers = read_headers(symbols / "types") if do_import else []
    to_ghidra = {"functions": import_rows(rows), "headers": headers} if do_import else None
    with tempfile.TemporaryDirectory(prefix="ghidrasync-") as tmp:
        exported = run_scripts(project, target, Path(tmp), to_ghidra, do_export)
    if exported is not None:
        write_if_changed(csv_path, format_functions(merge_functions(rows, exported["functions"])))
        write_types(symbols / "types", exported["types"])
        print(f"exported {len(exported['functions'])} functions to {csv_path} and "
              f"{len(exported['types'])} headers to {symbols / 'types'}")
    if do_import:
        print(f"imported {len(import_rows(rows))} functions, {len(headers)} headers into {project}")


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
