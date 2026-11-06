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
UNKNOWN_BESIDE_NID = re.compile(
    r"(?:\b(" + NID + r")\b[^\n]{0,40}\bname unknown\b|\bName unknown \(NID (" + NID + r")\))")


def known_names() -> dict[str, str]:
    with (ROOT / "symbols" / "nids.csv").open(newline="") as f:
        return {row["nid"]: row["name"] for row in csv.DictReader(f)}


def sources() -> list[Path]:
    return sorted(p for p in (ROOT / "src" / "hle").rglob("*") if p.suffix in (".cpp", ".hpp"))


class HleNameCommentsTest(unittest.TestCase):
    def test_unknown_name_comments_have_no_row(self) -> None:
        names = known_names()
        stale = []
        for path in sources():
            for number, line in enumerate(path.read_text().splitlines(), 1):
                for match in UNKNOWN_BESIDE_NID.finditer(line):
                    nid = match.group(1) or match.group(2)
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
