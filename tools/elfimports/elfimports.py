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
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "common"))
from toolenv import PinnedError, analysis_dir, is_inside, load_game_dir, read_pinned  # noqa: E402
from nid import NID_ALPHABET, is_nid  # noqa: E402
from ps4elf import (  # noqa: E402
    DT_NEEDED, DT_SCE_EXPORT_LIB, DT_SCE_EXPORT_LIB_ATTR, DT_SCE_IMPORT_LIB,
    DT_SCE_IMPORT_LIB_ATTR, DT_SCE_MODULE_INFO, DT_SCE_NEEDED_MODULE, DT_SONAME, ET_SCE_DYNAMIC,
    PINNED_ELFS, DynImage, ElfError, need, parse,
)

DEFAULT_IN = analysis_dir() / "elf"
DEFAULT_OUT = analysis_dir() / "imports"
KNOWN_FILES = PINNED_ELFS

# Library and module ids in symbol names use the same base64 digits as NIDs.
ID_ALPHABET = NID_ALPHABET

STT_NAMES = {0: "notype", 1: "object", 2: "func", 6: "tls"}
STB_NAMES = {0: "local", 1: "global", 2: "weak"}
STT_SECTION = 3
STB_WEAK = 2
SHN_UNDEF = 0
PLAIN_NAMES = {"module_start", "module_stop"}


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
    plain_symbols: list[str] = field(default_factory=list)  # module_start, module_stop


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

    # In all pinned files the needed modules are numbered 1..n in DT_NEEDED order. Module names
    # can differ from the file names, so without that numbering there is no safe pairing.
    need(sorted(modules) == list(range(1, len(needed) + 1)),
         "needed module records are not numbered 1..n in DT_NEEDED order")
    for mod_id, file_name in enumerate(needed, start=1):
        modules[mod_id].needed = file_name

    info = ElfInfo(name, image.e_type == ET_SCE_DYNAMIC, soname, self_module, modules, libraries)
    for sym in image.symbols[1:]:
        if sym.type == STT_SECTION and not sym.name:
            continue  # one unnamed local section symbol per file
        parts = sym.name.decode("ascii").split("#")
        if len(parts) != 3:
            need(len(parts) == 1 and sym.name != b"", f"malformed symbol name {sym.name!r}")
            # The only plain names in the pinned files are weak, untyped references with value 0.
            # Anything else might be an import we would silently drop, so refuse it.
            need(parts[0] in PLAIN_NAMES and sym.bind == STB_WEAK and sym.shndx == SHN_UNDEF,
                 f"unexpected symbol {sym.name!r} without a NID")
            info.plain_symbols.append(parts[0])
            continue
        nid, lib_text, mod_text = parts
        need(is_nid(nid), f"malformed NID in {sym.name!r}")
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
#
# An import names the module it comes from, and every bundled PRX exports its libraries under its
# own module name. So an import is resolved by (module, library, NID), and only against the PRXs
# that the importing file loads through DT_NEEDED.

Key = tuple[str, str, str]  # (module, library, NID)


def key(entry: Entry) -> Key:
    return entry.module.name, entry.library.name, entry.nid


def providers(prxs: list[ElfInfo]) -> dict[Key, str]:
    """(module, library, NID) -> name of the bundled PRX that exports it."""
    owner: dict[tuple[str, str], str] = {}
    out: dict[Key, str] = {}
    for info in prxs:
        for entry in info.exports:
            mod, lib, _ = key(entry)
            first = owner.setdefault((mod, lib), info.name)
            need(first == info.name, f"library {lib} of module {mod} is exported by {first} "
                                     f"and {info.name}")
            out[key(entry)] = info.name
    return out


def closure(info: ElfInfo, infos: list[ElfInfo]) -> list[ElfInfo]:
    """The file plus the given PRXs it pulls in through DT_NEEDED, transitively."""
    by_soname = {i.soname: i for i in infos if i.is_prx and i.soname}
    todo = [info]
    out: list[ElfInfo] = []
    while todo:
        cur = todo.pop(0)
        if cur in out:
            continue
        out.append(cur)
        todo += [by_soname[m.needed] for m in cur.modules.values() if m.needed in by_soname]
    return out


def static_load_set(infos: list[ElfInfo]) -> list[ElfInfo]:
    """The executable plus the bundled PRXs it pulls in through DT_NEEDED, transitively."""
    loaded: list[ElfInfo] = []
    for exe in (i for i in infos if not i.is_prx):
        loaded += [i for i in closure(exe, infos) if i not in loaded]
    return loaded


def resolvers(infos: list[ElfInfo]) -> dict[str, dict[Key, str]]:
    """File name -> exports of the PRXs that file loads (its closure without itself)."""
    return {info.name: providers(closure(info, infos)[1:]) for info in infos}


def write_csv(path: Path, info: ElfInfo, provided: dict[Key, str]) -> None:
    with path.open("w", newline="") as fh:
        out = csv.writer(fh)
        out.writerow(["direction", "nid", "library", "module", "type", "bind", "provided_by",
                      "value"])
        for entry in info.imports:
            out.writerow(["import", entry.nid, entry.library.name, entry.module.name, entry.type,
                          entry.bind, provided.get(key(entry), "firmware"), ""])
        for entry in info.exports:
            out.writerow(["export", entry.nid, entry.library.name, entry.module.name, entry.type,
                          entry.bind, info.name, f"{entry.value:#x}"])


def _counts(entries: list[Entry]) -> Counter:
    """Symbol type per unique NID. A NID can appear twice, e.g. as a weak and a global entry. A NID
    seen with two types counts once, as "mixed", so the counts add up to the number of NIDs."""
    types: dict[str, set[str]] = defaultdict(set)
    for e in entries:
        types[e.nid].add(e.type)
    return Counter(next(iter(t)) if len(t) == 1 else "mixed" for t in types.values())


def _types(counts: Counter) -> str:
    order = ["func", "object", "tls"]
    keys = order + sorted(k for k in counts if k not in order)
    return ", ".join(f"{counts[k]} {k}" for k in keys if counts[k]) or "none"


def _nids(entries: list[Entry]) -> int:
    return len({e.nid for e in entries})


def summary(infos: list[ElfInfo]) -> str:
    resolved = resolvers(infos)
    everything = providers([i for i in infos if i.is_prx])
    owner = {(mod, lib): prx for (mod, lib, _), prx in everything.items()}
    exported: Counter = Counter((mod, lib) for mod, lib, _ in everything)

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

    # Firmware scope: imports of the static load set that no PRX loaded by the file provides.
    # Grouped by (library, module): the same library name may come from two modules.
    scope: dict[tuple[str, str], list[Entry]] = defaultdict(list)
    users: dict[tuple[str, str], set[str]] = defaultdict(set)
    bundled_use: dict[tuple[str, str], list[Entry]] = defaultdict(list)
    for info in loaded:
        for entry in info.imports:
            group = entry.library.name, entry.module.name
            if key(entry) in resolved[info.name]:
                bundled_use[group].append(entry)
            else:
                scope[group].append(entry)
                users[group].add(info.name)

    total = sum(_nids(es) for es in scope.values())
    lines += ["", "## Firmware imports of the static load set", "",
              f"{total} NIDs in {len(scope)} libraries.", "",
              "| Library | Module | NIDs | Types | Imported by |", "|---|---|---|---|---|"]
    for group in sorted(scope, key=lambda g: (-_nids(scope[g]), g)):
        lib, mod = group
        lines.append(f"| {lib} | {mod} | {_nids(scope[group])} | "
                     f"{_types(_counts(scope[group]))} | {', '.join(sorted(users[group]))} |")

    lines += ["", "## Bundled libraries used by the static load set", "",
              "| Library | Provided by | NIDs used | NIDs exported |", "|---|---|---|---|"]
    for lib, mod in sorted(bundled_use):
        lines.append(f"| {lib} | {owner[mod, lib]} | {_nids(bundled_use[lib, mod])} | "
                     f"{exported[mod, lib]} |")

    # Imports from a library that a given PRX exports, but that the file doesn't get from it:
    # the PRX doesn't export that NID, or the file doesn't load the PRX.
    missing = [(info.name, e.library.name) for info in infos for e in info.imports
               if (e.module.name, e.library.name) in owner and key(e) not in resolved[info.name]]
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
            firmware = {key(e) for e in info.imports} - resolved[info.name].keys()
            libs = Counter(lib for _, lib, _ in firmware)
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
            known, data = read_pinned(path, KNOWN_FILES)
            infos.append(analyze(known, parse(data)))
        except (OSError, PinnedError, ElfError, UnicodeDecodeError) as exc:
            print(f"{path}: error: {exc}", file=sys.stderr)
            failed += 1
    if failed:
        # The cross-file results would be wrong with a file missing, so write nothing.
        return 1

    names = [info.name for info in infos]
    if len(set(names)) != len(names):
        print("error: the same file was given twice", file=sys.stderr)
        return 1

    try:
        text = summary(infos)
    except ElfError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    resolved = resolvers(infos)

    out_dir.mkdir(parents=True, exist_ok=True)
    # Drop outputs of earlier runs, which may cover other files. Only our own file names.
    for stale in [Path(n).stem + ".csv" for n in KNOWN_FILES.values()] + ["summary.md"]:
        (out_dir / stale).unlink(missing_ok=True)
    for info in infos:
        write_csv(out_dir / (Path(info.name).stem + ".csv"), info, resolved[info.name])
    (out_dir / "summary.md").write_text(text)
    print(text, end="")
    return 0


if __name__ == "__main__":
    sys.exit(main())
