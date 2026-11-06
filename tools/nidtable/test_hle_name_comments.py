#!/usr/bin/env python3
"""Checks that the HLE sources' name comments agree with symbols/nids.csv.

A comment that calls a NID's name unknown must not have a row in symbols/nids.csv, and a name
written next to a registration must be the row's name. Run: python3 -m unittest discover -s tools/nidtable
"""

from __future__ import annotations

import csv
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NID = r"[A-Za-z0-9+_-]{11}"
REGISTRATION = re.compile(r'^BB_HLE_FUNCTION\("[^"]+", "(' + NID + r')",[^;]*\);\s*(?://\s*(.*))?$')
NAME_ONLY = re.compile(r"^// (\w+)$")
# A NID may start or end with '+' or '-', where \b doesn't hold, so the NID is delimited by lookarounds.
NID_START = r"(?<![A-Za-z0-9+_-])"
NID_END = r"(?![A-Za-z0-9+_-])"
UNKNOWN_BESIDE_NID = re.compile(
    NID_START + "(" + NID + ")" + NID_END + r"[^\n]{0,40}\bname unknown\b|\bName unknown \(NID (" + NID + r")\)")


def unknown_name_nids(line: str) -> list[str]:
    """NIDs that the line's comment calls unknown.

    A registration line carries its NID in the call, so only its trailing comment is checked, however long
    the function name is. Other lines are searched for a NID followed closely by "name unknown".
    """
    registration = REGISTRATION.match(line)
    if registration:
        nid, trailing = registration.groups()
        return [nid] if trailing and re.match(r"name unknown\b", trailing.strip()) else []
    return [m.group(1) or m.group(2) for m in UNKNOWN_BESIDE_NID.finditer(line)]


def known_names() -> dict[str, str]:
    with (ROOT / "symbols" / "nids.csv").open(newline="") as f:
        return {row["nid"]: row["name"] for row in csv.DictReader(f)}


def sources() -> list[Path]:
    return sorted(p for p in (ROOT / "src" / "hle").rglob("*") if p.suffix in (".cpp", ".hpp"))


class UnknownNameCommentPatternTest(unittest.TestCase):
    def nids_in(self, line: str) -> list[str]:
        return unknown_name_nids(line)

    def test_finds_registrations_with_long_function_names(self) -> None:
        line = ('BB_HLE_FUNCTION("libkernel", "959qrazPIrg", bb::hle::kernel::process_parameters);'
                '  // name unknown')
        self.assertEqual(self.nids_in(line), ["959qrazPIrg"])
        self.assertEqual(self.nids_in(line.replace("// name unknown", "// scePthreadCreate")), [])

    def test_finds_nids_that_start_or_end_with_punctuation(self) -> None:
        for nid in ("-2IRUCO--PM", "-+3hY+y8bNo", "2SKEx6bSq-4", "4J2sUJmuHZQ"):
            self.assertEqual(self.nids_in(f'BB_HLE_FUNCTION("libkernel", "{nid}", f);  // name unknown'), [nid])
            self.assertEqual(self.nids_in(f"// {nid}, name unknown:"), [nid])
            self.assertEqual(self.nids_in(f"// Name unknown (NID {nid})"), [nid])

    def test_ignores_a_longer_run_of_nid_characters(self) -> None:
        self.assertEqual(self.nids_in("// x-2IRUCO--PMy, name unknown"), [])


class HleNameCommentsTest(unittest.TestCase):
    def test_unknown_name_comments_have_no_row(self) -> None:
        names = known_names()
        stale = []
        for path in sources():
            for number, line in enumerate(path.read_text().splitlines(), 1):
                for nid in unknown_name_nids(line):
                    if nid in names:
                        stale.append(f"{path.relative_to(ROOT)}:{number}: {nid} is {names[nid]}")
        self.assertEqual(stale, [], "comments say the name is unknown, but nids.csv has it")

    def test_registration_comments_match_the_table(self) -> None:
        names = known_names()
        wrong = []
        for path in sources():
            lines = path.read_text().splitlines()
            for number, line in enumerate(lines, 1):
                match = REGISTRATION.match(line)
                if not match:
                    continue
                nid, trailing = match.groups()
                before = NAME_ONLY.match(lines[number - 2]) if number > 1 else None
                written = trailing.strip() if trailing else (before.group(1) if before else "")
                if written and written != "name unknown" and re.fullmatch(r"\w+", written):
                    if names.get(nid) != written:
                        wrong.append(f"{path.relative_to(ROOT)}:{number}: {nid} is "
                                     f"{names.get(nid)}, the comment says {written}")
        self.assertEqual(wrong, [])


if __name__ == "__main__":
    unittest.main()
