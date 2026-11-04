#!/usr/bin/env python3
"""List the modules, libraries, imports and exports of the pinned PS4 ELFs.

Input is the ELFs rebuilt by tools/self2elf (the eboot and the PRXs bundled in sce_module/).
For each file the tool decodes the Sony module and library records of the dynamic table and
the NID#library#module symbol names. Across the files it works out which imports the bundled
PRXs provide and which must come from the system firmware. The format is described in
docs/formats/elf-dynamic.md.

Output, all under analysis/imports/ (untracked, never commit it):
    <file>.csv    one row per imported or exported symbol, with its NID
    summary.md    counts per library, also printed to stdout

Usage:
    elfimports.py                    # the 8 pinned ELFs in analysis/elf/
    elfimports.py FILE [FILE ...]    # specific files (cross-file results cover only these)
    elfimports.py -o DIR ...         # output directory (default: analysis/imports/)
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "common"))
from toolenv import analysis_dir, is_inside, load_game_dir  # noqa: E402
from ps4elf import (  # noqa: E402
    DT_NEEDED, DT_SCE_EXPORT_LIB, DT_SCE_EXPORT_LIB_ATTR, DT_SCE_IMPORT_LIB,
    DT_SCE_IMPORT_LIB_ATTR, DT_SCE_MODULE_INFO, DT_SCE_NEEDED_MODULE, DT_SONAME, ET_SCE_DYNAMIC,
    PINNED_ELFS, DynImage, ElfError, need, parse,
)

DEFAULT_IN = analysis_dir() / "elf"
DEFAULT_OUT = analysis_dir() / "imports"
KNOWN_FILES = PINNED_ELFS

# Digits of the library and module ids in symbol names, and the NID characters.
ID_ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-"
NID_LENGTH = 11

STT_NAMES = {0: "notype", 1: "object", 2: "func", 6: "tls"}
STB_NAMES = {0: "local", 1: "global", 2: "weak"}
STT_SECTION = 3
SHN_UNDEF = 0


@dataclass
class Module:
    id: int
    name: str
    version: tuple[int, int]  # (major, minor), our reading of the two version bytes
    needed: str | None = None  # the matching DT_NEEDED file name


@dataclass
class Library:
    id: int
    name: str
    version: int
    exported: bool
    attr: int | None = None
    module: Module | None = None  # from the symbols that use it


@dataclass
class Entry:
    nid: str
    library: Library
    module: Module
    type: str
    bind: str
    value: int


@dataclass
class ElfInfo:
    name: str  # pinned file name, e.g. eboot.elf
    is_prx: bool
    soname: str | None
    self_module: Module
    modules: dict[int, Module]  # needed modules by id
    libraries: dict[tuple[bool, int], Library]  # by (exported, id)
    imports: list[Entry] = field(default_factory=list)
    exports: list[Entry] = field(default_factory=list)
    plain_symbols: list[str] = field(default_factory=list)  # e.g. module_start


def decode_id(text: str) -> int:
    # Every id in the pinned files is a single digit (at most 42). How larger ids are written
    # is unknown, so refuse them instead of guessing.
    need(len(text) == 1 and text in ID_ALPHABET, f"unsupported id {text!r} in a symbol name")
    return ID_ALPHABET.index(text)


def _record(value: int) -> tuple[int, int, int]:
    """Split a packed module or library record into (id, version bits, string offset)."""
    return value >> 48, (value >> 32) & 0xFFFF, value & 0xFFFFFFFF


def analyze(name: str, image: DynImage) -> ElfInfo:
    def text(offset: int) -> str:
        return image.string(offset).decode("ascii")

    needed: list[str] = []
    soname = None
    self_module: Module | None = None
    modules: dict[int, Module] = {}
    libraries: dict[tuple[bool, int], Library] = {}
    attrs: list[tuple[bool, int, int]] = []

    for tag, value in image.entries:
        if tag == DT_NEEDED:
            needed.append(text(value))
        elif tag == DT_SONAME:
            soname = text(value)
        elif tag in (DT_SCE_MODULE_INFO, DT_SCE_NEEDED_MODULE):
            mod_id, version, name_off = _record(value)
            module = Module(mod_id, text(name_off), (version >> 8, version & 0xFF))
            if tag == DT_SCE_MODULE_INFO:
                need(self_module is None and mod_id == 0, "unexpected module info record")
                self_module = module
            else:
                need(mod_id != 0 and mod_id not in modules, f"duplicate module id {mod_id}")
                modules[mod_id] = module
        elif tag in (DT_SCE_EXPORT_LIB, DT_SCE_IMPORT_LIB):
            exported = tag == DT_SCE_EXPORT_LIB
            lib_id, version, name_off = _record(value)
            need((exported, lib_id) not in libraries, f"duplicate library id {lib_id}")
            libraries[exported, lib_id] = Library(lib_id, text(name_off), version, exported)
        elif tag in (DT_SCE_EXPORT_LIB_ATTR, DT_SCE_IMPORT_LIB_ATTR):
            lib_id, version, attr = _record(value)
            need(version == 0, f"unexpected bits in library attribute record {value:#x}")
            attrs.append((tag == DT_SCE_EXPORT_LIB_ATTR, lib_id, attr))
    need(self_module is not None, "no module info record")

    for exported, lib_id, attr in attrs:
        lib = libraries.get((exported, lib_id))
        need(lib is not None and lib.attr is None, f"attribute record for unknown library {lib_id}")
        lib.attr = attr

    # In all pinned files the needed modules are numbered 1..n in DT_NEEDED order.
    if sorted(modules) == list(range(1, len(needed) + 1)):
        for mod_id, file_name in enumerate(needed, start=1):
            modules[mod_id].needed = file_name

    info = ElfInfo(name, image.e_type == ET_SCE_DYNAMIC, soname, self_module, modules, libraries)
    for sym in image.symbols[1:]:
        if sym.type == STT_SECTION and not sym.name:
            continue  # one unnamed local section symbol per file
        parts = sym.name.decode("ascii").split("#")
        if len(parts) != 3:
            need(len(parts) == 1 and sym.name != b"", f"malformed symbol name {sym.name!r}")
            info.plain_symbols.append(parts[0])
            continue
        nid, lib_text, mod_text = parts
        need(len(nid) == NID_LENGTH and all(c in ID_ALPHABET for c in nid),
             f"malformed NID in {sym.name!r}")
        defined = sym.shndx != SHN_UNDEF
        lib = libraries.get((defined, decode_id(lib_text)))
        need(lib is not None, f"{sym.name!r} refers to an unknown library")
        mod_id = decode_id(mod_text)
        module = self_module if defined and mod_id == 0 else modules.get(mod_id)
        need(module is not None and (mod_id == 0) == defined,
             f"{sym.name!r} refers to an unknown module")
        need(lib.module in (None, module), f"library {lib.name} used with two modules")
        lib.module = module
        entry = Entry(nid, lib, module, STT_NAMES.get(sym.type, f"type{sym.type}"),
                      STB_NAMES.get(sym.bind, f"bind{sym.bind}"), sym.value)
        (info.exports if defined else info.imports).append(entry)
    return info


# --- across files -----------------------------------------------------------------------------

def providers(infos: list[ElfInfo]) -> dict[tuple[str, str], str]:
    """(library, NID) -> name of the bundled PRX that exports it."""
    out: dict[tuple[str, str], str] = {}
    for info in infos:
        for entry in info.exports:
            out.setdefault((entry.library.name, entry.nid), info.name)
    return out


def static_load_set(infos: list[ElfInfo]) -> list[ElfInfo]:
    """The executable plus the bundled PRXs it pulls in through DT_NEEDED, transitively."""
    by_soname = {info.soname: info for info in infos if info.is_prx and info.soname}
    todo = [info for info in infos if not info.is_prx]
    loaded: list[ElfInfo] = []
    while todo:
        info = todo.pop(0)
        if info in loaded:
            continue
        loaded.append(info)
        todo += [by_soname[m.needed] for m in info.modules.values() if m.needed in by_soname]
    return loaded


def write_csv(path: Path, info: ElfInfo, provided: dict[tuple[str, str], str]) -> None:
    with path.open("w", newline="") as fh:
        out = csv.writer(fh)
        out.writerow(["direction", "nid", "library", "module", "type", "bind", "provided_by",
                      "value"])
        for entry in info.imports:
            source = provided.get((entry.library.name, entry.nid), "firmware")
            out.writerow(["import", entry.nid, entry.library.name, entry.module.name, entry.type,
                          entry.bind, source, ""])
        for entry in info.exports:
            out.writerow(["export", entry.nid, entry.library.name, entry.module.name, entry.type,
                          entry.bind, info.name, f"{entry.value:#x}"])


def _counts(entries: list[Entry]) -> Counter:
    """Unique NIDs per symbol type. A NID can appear twice, e.g. as a weak and a global entry."""
    return Counter(t for _, t in {(e.nid, e.type) for e in entries})


def _types(counts: Counter) -> str:
    order = ["func", "object", "tls"]
    keys = order + sorted(k for k in counts if k not in order)
    return ", ".join(f"{counts[k]} {k}" for k in keys if counts[k]) or "none"


def summary(infos: list[ElfInfo], provided: dict[tuple[str, str], str]) -> str:
    lines = ["# Import summary", "",
             "Generated by tools/elfimports. Contains no NIDs. The full lists are in the CSVs.", "",
             "## Files", "",
             "| File | Module | Needed modules | Imported libraries | Imports | Exports |",
             "|---|---|---|---|---|---|"]
    for info in infos:
        imported_libs = sum(1 for (exp, _) in info.libraries if not exp)
        lines.append(f"| {info.name} | {info.self_module.name} | {len(info.modules)} | "
                     f"{imported_libs} | {_types(_counts(info.imports))} | "
                     f"{_types(_counts(info.exports))} |")

    loaded = static_load_set(infos)
    lines += ["", "## Static load set", "",
              "Loaded at startup through DT_NEEDED: " + ", ".join(i.name for i in loaded) + ".",
              "Not referenced through DT_NEEDED by any file in the set: "
              + (", ".join(i.name for i in infos if i not in loaded) or "none") + "."]

    # Firmware scope: imports of the static load set that no bundled PRX provides.
    scope: dict[str, list[Entry]] = defaultdict(list)
    users: dict[str, set[str]] = defaultdict(set)
    bundled_use: dict[str, list[Entry]] = defaultdict(list)
    for info in loaded:
        for entry in info.imports:
            if (entry.library.name, entry.nid) in provided:
                bundled_use[entry.library.name].append(entry)
            else:
                scope[entry.library.name].append(entry)
                users[entry.library.name].add(info.name)
    modules = {e.library.name: e.module.name for es in scope.values() for e in es}

    def nids(entries: list[Entry]) -> int:
        return len({e.nid for e in entries})

    total = sum(nids(es) for es in scope.values())
    lines += ["", "## Firmware imports of the static load set", "",
              f"{total} NIDs in {len(scope)} libraries.", "",
              "| Library | Module | NIDs | Types | Imported by |", "|---|---|---|---|---|"]
    for lib in sorted(scope, key=lambda k: (-nids(scope[k]), k)):
        lines.append(f"| {lib} | {modules[lib]} | {nids(scope[lib])} | "
                     f"{_types(_counts(scope[lib]))} | {', '.join(sorted(users[lib]))} |")

    lines += ["", "## Bundled libraries used by the static load set", "",
              "| Library | Provided by | NIDs used | NIDs exported |", "|---|---|---|---|"]
    exported: Counter = Counter(lib for lib, _ in provided)
    owner = {lib: prx for (lib, _), prx in provided.items()}
    for lib in sorted(bundled_use):
        lines.append(f"| {lib} | {owner[lib]} | {nids(bundled_use[lib])} | {exported[lib]} |")

    # Imports from a library that a bundled PRX exports but whose NID it doesn't export.
    missing = [(info.name, e.library.name) for info in infos for e in info.imports
               if e.library.name in owner and (e.library.name, e.nid) not in provided]
    lines += ["", "## Unresolved imports from bundled libraries", ""]
    if missing:
        lines += [f"- {file}: {n} from {lib}" for (file, lib), n in sorted(Counter(missing).items())]
    else:
        lines.append("None.")

    lines += ["", "## Declared libraries without imports", "",
              "Listed in the dynamic table, but no symbol refers to them.", ""]
    unused = [(info.name, lib.name) for info in infos for lib in info.libraries.values()
              if not lib.exported and lib.module is None]
    if unused:
        lines += [f"- {file}: {', '.join(sorted(lib for f, lib in unused if f == file))}"
                  for file in dict.fromkeys(f for f, _ in unused)]
    else:
        lines.append("None.")

    others = [info for info in infos if info not in loaded]
    if others:
        lines += ["", "## Firmware imports of the other PRXs", "",
                  "| File | Libraries |", "|---|---|"]
        for info in others:
            firmware = {(e.library.name, e.nid) for e in info.imports} - provided.keys()
            libs = Counter(lib for lib, _ in firmware)
            lines.append(f"| {info.name} | "
                         + ", ".join(f"{lib} ({n})" for lib, n in sorted(libs.items())) + " |")
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("files", nargs="*", type=Path,
                        help="ELF files from self2elf (default: the pinned ELFs in analysis/elf/)")
    parser.add_argument("-o", "--out-dir", type=Path, default=DEFAULT_OUT,
                        help="output directory (default: analysis/imports/ in the main checkout)")
    args = parser.parse_args(argv)

    files = args.files
    if not files:
        if not DEFAULT_IN.is_dir():
            parser.error(f"no files given and {DEFAULT_IN} does not exist (run self2elf first)")
        files = [DEFAULT_IN / name for name in sorted(set(KNOWN_FILES.values()))]

    out_dir = args.out_dir.expanduser().resolve()
    forbidden = {p.resolve().parent for p in files} | {DEFAULT_IN.resolve()}
    game_dir = load_game_dir()
    if game_dir is not None:
        forbidden.add(game_dir)
    for parent in sorted(forbidden):
        if is_inside(out_dir, parent):
            print(f"error: refusing to write into the game, input or self2elf directory ({parent})",
                  file=sys.stderr)
            return 2

    infos: list[ElfInfo] = []
    failed = 0
    for path in files:
        try:
            data = path.read_bytes()
            digest = hashlib.sha256(data).hexdigest()
            known = KNOWN_FILES.get(digest)
            if known is None:
                raise ElfError(f"not a pinned self2elf output (sha256 {digest})")
            infos.append(analyze(known, parse(data)))
        except (OSError, ElfError, UnicodeDecodeError) as exc:
            print(f"{path}: error: {exc}", file=sys.stderr)
            failed += 1
    if failed:
        # The cross-file results would be wrong with a file missing, so write nothing.
        return 1

    names = [info.name for info in infos]
    if len(set(names)) != len(names):
        print("error: the same file was given twice", file=sys.stderr)
        return 1

    provided = providers([info for info in infos if info.is_prx])
    out_dir.mkdir(parents=True, exist_ok=True)
    for info in infos:
        write_csv(out_dir / (Path(info.name).stem + ".csv"), info, provided)
    text = summary(infos, provided)
    (out_dir / "summary.md").write_text(text)
    print(text, end="")
    return 0


if __name__ == "__main__":
    sys.exit(main())
