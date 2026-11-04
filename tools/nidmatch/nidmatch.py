#!/usr/bin/env python3
"""Recover the names of the game's imported and exported NIDs by hashing candidate names.

A NID is a hash of a symbol name (see tools/common/nid.py). A candidate name is accepted only if
its NID equals one that the pinned ELFs import or export. This is the procedure in the NID
section of docs/clean-room.md. Every match records the candidate sources it came from.

Input:
    analysis/imports/*.csv        NIDs per file, written by tools/elfimports
    symbols/nids.csv              earlier matches, hashed again on every run
    symbols/candidates/*.txt      hand-picked names (committed), label = file stem
    analysis/nids/candidates/     generated lists (local, see syscandidates.py), label = file stem
    analysis/elf/*.elf            identifiers in the pinned ELFs, label = strings:<file stem>
    FILE ...                      extra candidate files, label = file stem

Candidate files hold one name per line. '#' starts a comment.

Output:
    symbols/nids.csv              one row per matched (NID, library), committed
    analysis/nids/unmatched.csv   NIDs still without a name (local, never commit it)
    analysis/nids/report.md       matched and total per library, also printed to stdout

Usage:
    nidmatch.py [FILE ...]
    nidmatch.py --no-write [FILE ...]   # report only, change nothing
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import re
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "common"))
from nid import is_nid, nid  # noqa: E402
from ps4elf import PINNED_ELFS  # noqa: E402
from toolenv import REPO_ROOT, analysis_dir  # noqa: E402

IMPORTS_DIR = analysis_dir() / "imports"
ELF_DIR = analysis_dir() / "elf"
OUT_DIR = analysis_dir() / "nids"
GENERATED_DIR = OUT_DIR / "candidates"
NIDS_CSV = REPO_ROOT / "symbols" / "nids.csv"
CURATED_DIR = REPO_ROOT / "symbols" / "candidates"

FILE_STEMS = sorted(Path(name).stem for name in PINNED_ELFS.values())
# The code loaded at startup: the eboot and the two bundled PRXs it needs (docs/formats/imports.md).
STATIC_LOAD_SET = ("eboot", "libc", "libSceFios2")

NIDS_FIELDS = ["nid", "library", "name", "source"]
NAME_RE = re.compile(r"[!-~]+")  # printable ASCII, no spaces
IDENT_RE = re.compile(rb"[A-Za-z_][A-Za-z0-9_]{2,}")


class MatchError(Exception):
    pass


@dataclass
class Use:
    """How one library's NID shows up across the pinned files."""
    module: str
    type: str
    importers: set[str] = field(default_factory=set)
    exporters: set[str] = field(default_factory=set)
    from_firmware: bool = False  # imported by some file and not provided by a bundled PRX


# nid -> library -> Use
NidTable = dict[str, dict[str, Use]]
# name -> source labels
Candidates = dict[str, set[str]]


def load_nids(imports_dir: Path) -> NidTable:
    table: NidTable = defaultdict(dict)
    for stem in FILE_STEMS:
        path = imports_dir / f"{stem}.csv"
        if not path.is_file():
            raise MatchError(f"{path} is missing (run tools/elfimports first)")
        with path.open(newline="") as f:
            for row in csv.DictReader(f):
                if not is_nid(row["nid"]):
                    raise MatchError(f"{path}: bad NID {row['nid']!r}")
                use = table[row["nid"]].setdefault(row["library"], Use(row["module"], row["type"]))
                if row["direction"] == "export":
                    use.exporters.add(stem)
                else:
                    use.importers.add(stem)
                    use.from_firmware |= row["provided_by"] == "firmware"
    return dict(table)


def add(candidates: Candidates, name: str, label: str, where: str) -> None:
    if not NAME_RE.fullmatch(name):
        raise MatchError(f"{where}: not a symbol name: {name!r}")
    candidates.setdefault(name, set()).add(label)


def read_candidate_file(path: Path, candidates: Candidates, label: str | None = None) -> None:
    label = label or path.stem
    for lineno, line in enumerate(path.read_text(encoding="ascii").splitlines(), 1):
        name = line.split("#", 1)[0].strip()
        if name:
            add(candidates, name, label, f"{path}:{lineno}")


def read_strings(elf_dir: Path, candidates: Candidates) -> None:
    """Identifier-like runs of bytes in the pinned ELFs. Any of them may be a symbol name."""
    for digest, file_name in sorted(PINNED_ELFS.items(), key=lambda kv: kv[1]):
        path = elf_dir / file_name
        data = path.read_bytes()
        if hashlib.sha256(data).hexdigest() != digest:
            raise MatchError(f"{path} is not the pinned self2elf output")
        label = f"strings:{path.stem}"
        for token in set(IDENT_RE.findall(data)):
            candidates.setdefault(token.decode("ascii"), set()).add(label)


def read_nids_csv(path: Path, table: NidTable, candidates: Candidates) -> None:
    """Earlier matches. Each must still hash to its NID and name a NID of the pinned files."""
    if not path.is_file():
        return
    with path.open(newline="") as f:
        reader = csv.DictReader(f)
        if reader.fieldnames != NIDS_FIELDS:
            raise MatchError(f"{path}: expected columns {','.join(NIDS_FIELDS)}")
        for lineno, row in enumerate(reader, 2):
            where = f"{path}:{lineno}"
            if nid(row["name"]) != row["nid"]:
                raise MatchError(f"{where}: {row['name']} does not hash to {row['nid']}")
            if row["library"] not in table.get(row["nid"], {}):
                raise MatchError(f"{where}: {row['nid']} is not a NID of {row['library']} "
                                 "in the pinned files")
            labels = [s for s in row["source"].split(";") if s]
            if not labels:
                raise MatchError(f"{where}: no source")
            for label in labels:
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


def nids_rows(table: NidTable, found: dict[str, tuple[str, set[str]]]) -> list[dict[str, str]]:
    rows = [{"nid": h, "library": lib, "name": name, "source": ";".join(sorted(labels))}
            for h, (name, labels) in found.items() for lib in table[h]]
    return sorted(rows, key=lambda r: (r["library"].lower(), r["name"], r["nid"]))


def unmatched_rows(table: NidTable, found: dict[str, tuple[str, set[str]]]) -> list[dict[str, str]]:
    rows = [{"nid": h, "library": lib, "module": use.module, "type": use.type,
             "importers": ";".join(sorted(use.importers)),
             "exporters": ";".join(sorted(use.exporters))}
            for h, libs in table.items() if h not in found for lib, use in libs.items()]
    return sorted(rows, key=lambda r: (r["library"].lower(), r["nid"]))


def report(table: NidTable, found: dict[str, tuple[str, set[str]]]) -> str:
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
            if use.from_firmware and use.importers & set(STATIC_LOAD_SET):
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
    args = parser.parse_args(argv)

    try:
        table = load_nids(IMPORTS_DIR)
        candidates: Candidates = {}
        read_nids_csv(NIDS_CSV, table, candidates)
        for directory in (CURATED_DIR, GENERATED_DIR):
            for path in sorted(directory.glob("*.txt")):
                read_candidate_file(path, candidates)
        for path in args.files:
            read_candidate_file(path, candidates)
        read_strings(ELF_DIR, candidates)
        found = match(table, candidates)
    except (OSError, UnicodeDecodeError, MatchError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    text = report(table, found)
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
