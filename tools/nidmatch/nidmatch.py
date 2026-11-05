#!/usr/bin/env python3
"""Recover the names of the game's imported and exported NIDs by hashing candidate names.

A NID is a hash of a symbol name (see tools/common/nid.py). A candidate name is accepted only if
its NID equals one that the pinned ELFs import or export. This is the procedure in the NID
section of docs/clean-room.md. Every match records the candidate sources it came from.

Input:
    analysis/elf/*.elf            the pinned ELFs, loaded and checked by tools/elfimports:
                                  their imports and exports, and their identifiers as candidates,
                                  label = strings:<file stem>
    symbols/nids.csv              earlier matches, hashed again on every run (see read_nids_csv)
    symbols/candidates/*.txt      hand-picked names (committed), label = file stem
    analysis/nids/candidates/     generated lists (local, see syscandidates.py and derive.py),
                                  label = file stem
    FILE ...                      extra candidate files, label = file stem

Candidate files hold one name per line. '#' starts a comment. A file's stem is its source label,
so it may use only letters, digits and '_.+-'.

Output:
    symbols/nids.csv              one row per matched (NID, library), committed
    analysis/nids/unmatched.csv   NIDs still without a name (local, never commit it)
    analysis/nids/report.md       matched and total per library, also printed to stdout

Usage:
    nidmatch.py [FILE ...]
    nidmatch.py --no-write [FILE ...]   # report only, change nothing
    nidmatch.py --withdraw LABEL        # drop a label from earlier matches (deleted source)
"""

from __future__ import annotations

import argparse
import csv
import re
import sys
from collections import defaultdict
from collections.abc import Iterator
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "common"))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "elfimports"))
import elfimports  # noqa: E402
from elfimports import ElfInfo, Entry, LoadError  # noqa: E402
from nid import nid  # noqa: E402
from ps4elf import ELF_DIR, ElfError  # noqa: E402
from toolenv import REPO_ROOT, analysis_dir  # noqa: E402

OUT_DIR = analysis_dir() / "nids"
GENERATED_DIR = OUT_DIR / "candidates"
NIDS_CSV = REPO_ROOT / "symbols" / "nids.csv"
CURATED_DIR = REPO_ROOT / "symbols" / "candidates"

NIDS_FIELDS = ["nid", "library", "name", "source"]
NAME_RE = re.compile(r"[!-~]+")  # printable ASCII, no spaces
IDENT_RE = re.compile(rb"[A-Za-z_][A-Za-z0-9_]{2,}")
# Labels are joined with ';' in nids.csv, and 'strings:' marks the labels of read_strings.
LABEL_RE = re.compile(r"[A-Za-z0-9_.+-]+")
STRINGS_PREFIX = "strings:"
# Labels of the lists written by derive.py.
DERIVED_PREFIX = "derive-"
# Libraries in the exposure log of docs/clean-room.md, plus
# libSceSaveDataDialog, whose names start with the libSceSaveData prefix. Derivation rules skip
# them, and a name for one of them needs a source other than a derivation rule.
NO_DERIVE_LIBRARIES = frozenset({"libSceRemoteplay", "libSceSaveData", "libSceRegMgr", "libSceRtc",
                                 "libSceSaveDataDialog"})


class MatchError(Exception):
    pass


# The errors that end a run of nidmatch or derive with a message instead of a traceback.
ERRORS = (LoadError, OSError, UnicodeError, MatchError, ElfError)


def print_error(exc: Exception) -> int:
    """Prints one of ERRORS and returns the exit status. LoadError has its own prefixes."""
    print(exc if isinstance(exc, LoadError) else f"error: {exc}", file=sys.stderr)
    return 1


@dataclass
class Use:
    """How one library's NID shows up across the pinned files."""
    module: str
    types: set[str] = field(default_factory=set)
    importers: set[str] = field(default_factory=set)
    exporters: set[str] = field(default_factory=set)
    # Importers that get the NID from the firmware, i.e. from no PRX they load. Kept per file: one
    # file may get a NID from a bundled PRX while another file without that PRX gets it from firmware.
    firmware_importers: set[str] = field(default_factory=set)


# nid -> library -> Use
NidTable = dict[str, dict[str, Use]]
# name -> source labels
Candidates = dict[str, set[str]]


def nids_csv_rows(path: Path) -> Iterator[tuple[str, dict[str, str]]]:
    """(file:line, row) for each row of nids.csv, after checking the columns and the field count.
    Nothing if the file does not exist."""
    if not path.is_file():
        return
    with path.open(newline="") as f:
        reader = csv.DictReader(f)
        if reader.fieldnames != NIDS_FIELDS:
            raise MatchError(f"{path}: expected columns {','.join(NIDS_FIELDS)}")
        for lineno, row in enumerate(reader, 2):
            # DictReader fills a short row with None values and puts the rest of a long row under
            # None.
            if None in row or None in row.values():
                raise MatchError(f"{path}:{lineno}: wrong number of fields")
            yield f"{path}:{lineno}", row


def load_nids(infos: list[ElfInfo]) -> NidTable:
    """The imports and exports of the pinned files, from the same analysis tools/elfimports runs."""
    resolved = elfimports.resolvers(infos)
    table: NidTable = defaultdict(dict)

    def use_of(entry: Entry, file_name: str) -> Use:
        # nids.csv has one row per (NID, library). That is enough only while each pair comes from
        # one module, which holds for all pinned files.
        use = table[entry.nid].setdefault(entry.library.name, Use(entry.module.name))
        if use.module != entry.module.name:
            raise MatchError(f"{file_name}: {entry.nid} of {entry.library.name} comes from "
                             f"modules {use.module} and {entry.module.name}")
        use.types.add(entry.type)
        return use

    for info in infos:
        stem = Path(info.name).stem
        for entry in info.imports:
            use = use_of(entry, info.name)
            use.importers.add(stem)
            if elfimports.key(entry) not in resolved[info.name]:
                use.firmware_importers.add(stem)
        for entry in info.exports:
            use_of(entry, info.name).exporters.add(stem)
    return dict(table)


def add(candidates: Candidates, name: str, label: str, where: str) -> None:
    if not NAME_RE.fullmatch(name):
        raise MatchError(f"{where}: not a symbol name: {name!r}")
    candidates.setdefault(name, set()).add(label)


def read_candidate_file(path: Path, candidates: Candidates) -> None:
    label = path.stem
    if not LABEL_RE.fullmatch(label):
        raise MatchError(f"{path}: the file name gives the source label {label!r}, "
                         "use only letters, digits and _.+-")
    for lineno, line in enumerate(path.read_text(encoding="ascii").splitlines(), 1):
        name = line.split("#", 1)[0].strip()
        if name:
            add(candidates, name, label, f"{path}:{lineno}")


def check_unique_labels(files: list[Path]) -> None:
    """Each candidate file must give its own label. Two files with one stem would share it, and
    read_nids_csv could not tell their names apart."""
    seen: dict[str, Path] = {}
    for path in files:
        first = seen.setdefault(path.stem, path)
        if first != path:
            raise MatchError(f"{first} and {path} both give the source label {path.stem!r}")


def read_strings(elfs: dict[str, bytes], candidates: Candidates) -> None:
    """Identifier-like runs of bytes in the pinned ELFs. Any of them may be a symbol name."""
    for file_name, data in elfs.items():
        label = STRINGS_PREFIX + Path(file_name).stem
        for token in set(IDENT_RE.findall(data)):
            candidates.setdefault(token.decode("ascii"), set()).add(label)


def read_nids_csv(path: Path, table: NidTable, candidates: Candidates, rederived: set[str]) -> None:
    """Earlier matches. Each must still hash to its NID and name a NID of the pinned files.

    A label is carried over from nids.csv only if this run does not read its source. The strings
    of the pinned ELFs are read on every run, and `rederived` holds the labels of the candidate
    files this run reads (committed, generated and extra), so their labels are not carried over:
    a name taken out of such a file, e.g. by a tightened rule in derive.py, loses that label.
    Labels of files missing from this run are carried over, so a run without them keeps their
    names. `rederived` also holds the labels passed to --withdraw. A row left without a label is
    dropped unless this run finds the name again.
    """
    for where, row in nids_csv_rows(path):
        if not NAME_RE.fullmatch(row["name"]):
            raise MatchError(f"{where}: not a symbol name: {row['name']!r}")
        if nid(row["name"]) != row["nid"]:
            raise MatchError(f"{where}: {row['name']} does not hash to {row['nid']}")
        if row["library"] not in table.get(row["nid"], {}):
            raise MatchError(f"{where}: {row['nid']} is not a NID of {row['library']} "
                             "in the pinned files")
        labels = [s for s in row["source"].split(";") if s]
        if not labels:
            raise MatchError(f"{where}: no source")
        for label in labels:
            if not label.startswith(STRINGS_PREFIX) and label not in rederived:
                add(candidates, row["name"], label, where)


def match(table: NidTable, candidates: Candidates) -> dict[str, tuple[str, set[str]]]:
    """nid -> (name, source labels). Two names with one NID would be a hash collision."""
    found: dict[str, tuple[str, set[str]]] = {}
    for name, labels in candidates.items():
        h = nid(name)
        if h not in table:
            continue
        if h in found and found[h][0] != name:
            raise MatchError(f"{found[h][0]} and {name} both hash to {h}")
        found[h] = (name, labels)
    return found


def check_derived(table: NidTable, found: dict[str, tuple[str, set[str]]]) -> None:
    """A derivation rule alone may not name a NID of the libraries in NO_DERIVE_LIBRARIES."""
    for h, (name, labels) in found.items():
        blocked = sorted(NO_DERIVE_LIBRARIES & table[h].keys())
        if blocked and all(label.startswith(DERIVED_PREFIX) for label in labels):
            raise MatchError(f"{name} names a NID of {', '.join(blocked)} but comes only from "
                             f"derivation rules ({';'.join(sorted(labels))})")


def nids_rows(table: NidTable, found: dict[str, tuple[str, set[str]]]) -> list[dict[str, str]]:
    rows = [{"nid": h, "library": lib, "name": name, "source": ";".join(sorted(labels))}
            for h, (name, labels) in found.items() for lib in table[h]]
    return sorted(rows, key=lambda r: (r["library"].lower(), r["name"], r["nid"]))


def unmatched_rows(table: NidTable, found: dict[str, tuple[str, set[str]]]) -> list[dict[str, str]]:
    rows = [{"nid": h, "library": lib, "module": use.module, "type": ";".join(sorted(use.types)),
             "importers": ";".join(sorted(use.importers)),
             "exporters": ";".join(sorted(use.exporters))}
            for h, libs in table.items() if h not in found for lib, use in libs.items()]
    return sorted(rows, key=lambda r: (r["library"].lower(), r["nid"]))


def report(table: NidTable, found: dict[str, tuple[str, set[str]]], load_set: set[str]) -> str:
    # library -> [nids, matched], counted once per (nid, library)
    stats: dict[str, list[int]] = defaultdict(lambda: [0, 0])
    exported: set[str] = set()
    static_total = static_matched = 0
    for h, libs in table.items():
        for lib, use in libs.items():
            stats[lib][0] += 1
            stats[lib][1] += h in found
            if use.exporters:
                exported.add(lib)
            if use.firmware_importers & load_set:
                static_total += 1
                static_matched += h in found

    def pct(m: int, n: int) -> str:
        return f"{100 * m / n:.0f}%" if n else "-"

    total = sum(n for n, _ in stats.values())
    matched = sum(m for _, m in stats.values())
    lines = [
        "# NID matching",
        "",
        f"Matched {matched} of {total} (NID, library) pairs ({pct(matched, total)}). "
        f"Firmware imports of the static load set: {static_matched} of {static_total} "
        f"({pct(static_matched, static_total)}).",
        "",
        "| Library | Provided by | NIDs | Matched | |",
        "|---|---|---|---|---|",
    ]
    for lib in sorted(stats, key=lambda name: (-stats[name][0], name.lower())):
        n, m = stats[lib]
        kind = "bundled PRX" if lib in exported else "firmware"
        lines.append(f"| {lib} | {kind} | {n} | {m} | {pct(m, n)} |")
    return "\n".join(lines) + "\n"


def write_list(path: Path, header: list[str], names: set[str]) -> None:
    """A candidate list: the header as comment lines, then the names sorted."""
    lines = [f"# {h}" for h in header] + sorted(names)
    path.write_text("\n".join(lines) + "\n", encoding="ascii")
    print(f"{path.name}: {len(names)} names")


def write_csv(path: Path, fields: list[str], rows: list[dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fields, lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("files", nargs="*", type=Path, help="extra candidate files")
    parser.add_argument("--no-write", action="store_true",
                        help="print the report, write nothing")
    parser.add_argument("--withdraw", action="append", default=[], metavar="LABEL",
                        help="drop LABEL from the earlier matches in nids.csv, e.g. after "
                             "deleting its candidate file (repeatable)")
    args = parser.parse_args(argv)

    try:
        infos, elfs = elfimports.load_dir(ELF_DIR)
        table = load_nids(infos)
        files = sorted(CURATED_DIR.glob("*.txt")) + sorted(GENERATED_DIR.glob("*.txt")) + args.files
        check_unique_labels(files)
        candidates: Candidates = {}
        read_nids_csv(NIDS_CSV, table, candidates,
                      {path.stem for path in files} | set(args.withdraw))
        for path in files:
            read_candidate_file(path, candidates)
        read_strings(elfs, candidates)
        found = match(table, candidates)
        check_derived(table, found)
        load_set = {Path(info.name).stem for info in elfimports.static_load_set(infos)}
    except ERRORS as exc:
        return print_error(exc)

    text = report(table, found, load_set)
    if not args.no_write:
        write_csv(NIDS_CSV, NIDS_FIELDS, nids_rows(table, found))
        write_csv(OUT_DIR / "unmatched.csv",
                  ["nid", "library", "module", "type", "importers", "exporters"],
                  unmatched_rows(table, found))
        (OUT_DIR / "report.md").write_text(text)
    print(text, end="")
    return 0


if __name__ == "__main__":
    sys.exit(main())
