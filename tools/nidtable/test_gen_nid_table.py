#!/usr/bin/env python3
"""Tests for gen_nid_table. Run: python3 -m unittest discover -s tools/nidtable"""

from __future__ import annotations

import sys
import tempfile
import unittest
from unittest import mock
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen_nid_table as g  # noqa: E402

HEADER = "nid,library,name,source\n"


class GenNidTableTest(unittest.TestCase):
    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.dir = Path(tmp.name)

    def run_main(self, csv_text: str) -> tuple[int, str]:
        (self.dir / "nids.csv").write_text(csv_text)
        out = self.dir / "table.cpp"
        out.unlink(missing_ok=True)
        status = g.main(["gen", str(self.dir / "nids.csv"), str(out)])
        return status, out.read_text() if out.exists() else ""

    def test_sorted_and_deduplicated(self) -> None:
        status, text = self.run_main(HEADER + "zzzzzzzzzzz,libc,last,glibc\n"
                                     "+AAAAAAAAAA,libc,first,glibc\n"
                                     "zzzzzzzzzzz,libSceLibcInternal,last,glibc\n")
        self.assertEqual(status, 0)
        self.assertIn("std::array<NidName, 2>", text)
        first = text.index('{"+AAAAAAAAAA", "first"},')
        self.assertLess(first, text.index('{"zzzzzzzzzzz", "last"},'))

    def test_escapes_quotes_and_backslashes(self) -> None:
        self.assertEqual(g.cpp_string('a"b\\c'), '"a\\"b\\\\c"')

    def test_empty_table(self) -> None:
        status, text = self.run_main(HEADER)
        self.assertEqual(status, 0)
        self.assertIn("std::array<NidName, 0>", text)

    def test_rejects_bad_input(self) -> None:
        for csv_text in ["nid,name\n",
                         HEADER + "short,libc,f,glibc\n",
                         HEADER + "AAAAAAAAAAA,libc,has space,glibc\n",
                         HEADER + "AAAAAAAAAAA,libc,f\n",
                         HEADER + "AAAAAAAAAAA,libc,f,glibc,extra\n",
                         HEADER + "AAAAAAAAAAA,libc,f,glibc\nAAAAAAAAAAA,libc,g,glibc\n"]:
            with self.subTest(csv_text=csv_text):
                with mock.patch("sys.stderr"):
                    status, text = self.run_main(csv_text)
                self.assertEqual(status, 1)
                self.assertEqual(text, "")


if __name__ == "__main__":
    unittest.main()
