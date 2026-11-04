#!/usr/bin/env python3
"""Tests for nid and nidmatch. Run: python3 -m unittest discover -s tools/nidmatch

The synthetic tests need no game files. The game test runs only when BB_GAME_DIR is set and the
outputs of self2elf and elfimports exist in analysis/.
"""

from __future__ import annotations

import contextlib
import csv
import hashlib
import io
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS / "nidmatch"))
sys.path.insert(0, str(TOOLS / "common"))
import nidmatch as m  # noqa: E402
from nid import NID_ALPHABET, is_nid, nid  # noqa: E402
from toolenv import load_game_dir  # noqa: E402

# Exports of the pinned libc.prx, named by matching. strlen (0x2f970) and strcmp (0x30160) were
# also checked against their disassembly. Derived by us, see docs/formats/nid.md.
KNOWN = {
    "strlen": "j4ViWNHEgww",
    "strcmp": "Ovb2dSJOAuE",
    "memset": "8zTFvBIAIN8",
}

IMPORT_FIELDS = ["direction", "nid", "library", "module", "type", "bind", "provided_by", "value"]


class NidTest(unittest.TestCase):
    def test_known_pairs(self):
        for name, expected in KNOWN.items():
            self.assertEqual(nid(name), expected, name)
            self.assertEqual(nid(name.encode()), expected, name)

    def test_shape(self):
        for name in ["a", "_Znwm", "sceKernelSomething", "x" * 300]:
            h = nid(name)
            self.assertTrue(is_nid(h), h)
            # 64 bits in 11 digits: the last digit carries 4 bits, its low 2 bits are 0.
            self.assertEqual(NID_ALPHABET.index(h[-1]) & 3, 0, h)

    def test_rejects_non_ascii(self):
        with self.assertRaises(UnicodeEncodeError):
            nid("stré")

    def test_is_nid(self):
        self.assertTrue(is_nid("AAAAAAAAAAA"))
        self.assertFalse(is_nid("AAAAAAAAAA"))
        self.assertFalse(is_nid("AAAAAAAAAA/"))


class Fixture:
    """A fake analysis/ and symbols/ tree with two pinned files, 'main' and 'lib'."""

    def __init__(self, root: Path):
        self.root = root
        self.imports = root / "imports"
        self.elf = root / "elf"
        self.out = root / "nids"
        self.curated = root / "curated"
        self.nids_csv = root / "nids.csv"
        for d in (self.imports, self.elf, self.curated):
            d.mkdir()
        self.rows: dict[str, list[dict[str, str]]] = {"main": [], "lib": []}
        self.elf_data = {"main": b"\0\0alpha_func\0xx\0", "lib": b"\0beta\0"}

    def row(self, stem: str, direction: str, name: str, library: str,
            provided_by: str = "firmware", module: str | None = None) -> Fixture:
        self.rows[stem].append({
            "direction": direction, "nid": nid(name), "library": library,
            "module": module or library,
            "type": "func", "bind": "global",
            "provided_by": "" if direction == "export" else provided_by,
            "value": "0x10" if direction == "export" else "",
        })
        return self

    def run(self, *args: str) -> tuple[int, str, str]:
        pinned = {}
        for stem, rows in self.rows.items():
            with (self.imports / f"{stem}.csv").open("w", newline="") as f:
                w = csv.DictWriter(f, fieldnames=IMPORT_FIELDS, lineterminator="\n")
                w.writeheader()
                w.writerows(rows)
            (self.elf / f"{stem}.elf").write_bytes(self.elf_data[stem])
            pinned[hashlib.sha256(self.elf_data[stem]).hexdigest()] = f"{stem}.elf"
        out, err = io.StringIO(), io.StringIO()
        with contextlib.ExitStack() as stack:
            for attr, value in [("IMPORTS_DIR", self.imports), ("ELF_DIR", self.elf),
                                ("OUT_DIR", self.out), ("GENERATED_DIR", self.out / "candidates"),
                                ("NIDS_CSV", self.nids_csv), ("CURATED_DIR", self.curated),
                                ("PINNED_ELFS", pinned), ("FILE_STEMS", sorted(self.rows)),
                                # The fake ELFs have no dynamic table to work the load set out of.
                                ("static_load_set", lambda elfs: {"main"})]:
                stack.enter_context(mock.patch.object(m, attr, value))
            stack.enter_context(contextlib.redirect_stdout(out))
            stack.enter_context(contextlib.redirect_stderr(err))
            code = m.main(list(args))
        return code, out.getvalue(), err.getvalue()

    def nids(self) -> list[dict[str, str]]:
        with self.nids_csv.open(newline="") as f:
            return list(csv.DictReader(f))


class MatchTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.fx = Fixture(Path(tmp.name))
        (self.fx.row("main", "import", "alpha_func", "libA")
                .row("main", "import", "gamma", "libB", provided_by="lib.elf")
                .row("main", "import", "unknown_one", "libA")
                .row("lib", "export", "gamma", "libB"))

    def test_matches_strings_and_files(self):
        (self.fx.curated / "hand.txt").write_text("# comment\ngamma  # used like this\n\n")
        code, out, err = self.fx.run()
        self.assertEqual(code, 0, err)
        self.assertEqual(self.fx.nids(), [
            {"nid": nid("alpha_func"), "library": "libA", "name": "alpha_func",
             "source": "strings:main"},
            {"nid": nid("gamma"), "library": "libB", "name": "gamma", "source": "hand"},
        ])
        with (self.fx.out / "unmatched.csv").open(newline="") as f:
            unmatched = list(csv.DictReader(f))
        self.assertEqual([r["nid"] for r in unmatched], [nid("unknown_one")])
        self.assertIn("Matched 2 of 3", out)
        self.assertIn("static load set: 1 of 2", out)
        self.assertIn("| libB | bundled PRX | 1 | 1 | 100% |", out)

    def test_earlier_matches_are_kept_and_merged(self):
        extra = self.fx.root / "extra.txt"
        extra.write_text("unknown_one\n")
        self.assertEqual(self.fx.run(str(extra))[0], 0)
        extra.unlink()
        (self.fx.curated / "hand.txt").write_text("unknown_one\n")
        self.assertEqual(self.fx.run()[0], 0)
        row = next(r for r in self.fx.nids() if r["name"] == "unknown_one")
        self.assertEqual(row["source"], "extra;hand")

    def test_rederived_labels_are_not_carried_over(self):
        (self.fx.curated / "hand.txt").write_text("unknown_one\n")
        self.fx.nids_csv.write_text("nid,library,name,source\n"
                                    f"{nid('alpha_func')},libA,alpha_func,strings:main;hand\n")
        self.assertEqual(self.fx.run()[0], 0)
        # 'hand' no longer lists alpha_func, and strings:main is found again, not carried over.
        self.assertEqual({r["name"]: r["source"] for r in self.fx.nids()},
                         {"alpha_func": "strings:main", "unknown_one": "hand"})
        # Taking the name out of the committed file drops it.
        (self.fx.curated / "hand.txt").write_text("# withdrawn\n")
        self.assertEqual(self.fx.run()[0], 0)
        self.assertEqual([r["name"] for r in self.fx.nids()], ["alpha_func"])

    def test_withdraw(self):
        extra = self.fx.root / "extra.txt"
        extra.write_text("unknown_one\nalpha_func\n")
        self.assertEqual(self.fx.run(str(extra))[0], 0)
        extra.unlink()
        self.assertEqual(self.fx.run()[0], 0)  # kept without the file
        self.assertEqual(len(self.fx.nids()), 2)
        self.assertEqual(self.fx.run("--withdraw", "extra")[0], 0)
        self.assertEqual({r["name"]: r["source"] for r in self.fx.nids()},
                         {"alpha_func": "strings:main"})

    def test_no_write(self):
        code, out, _ = self.fx.run("--no-write")
        self.assertEqual(code, 0)
        self.assertFalse(self.fx.nids_csv.exists())
        self.assertFalse(self.fx.out.exists())
        self.assertIn("Matched 1 of 3", out)

    def test_bad_rows_in_nids_csv(self):
        bad = [
            (f"{nid('beta')},libA,alpha_func,x\n", "does not hash"),
            (f"{nid('beta')},libA,beta,x\n", "is not a NID of libA"),
            (f"{nid('alpha_func')},libA,alpha_func,\n", "no source"),
            (f"{nid('alpha_func')},libA\n", "wrong number of fields"),
            (f"{nid('alpha_func')},libA,alpha_func,x,y\n", "wrong number of fields"),
        ]
        for line, message in bad:
            self.fx.nids_csv.write_text("nid,library,name,source\n" + line)
            code, _, err = self.fx.run()
            self.assertEqual(code, 1, line)
            self.assertIn(message, err)

    def test_bad_candidate_name(self):
        (self.fx.curated / "hand.txt").write_text("two words\n")
        code, _, err = self.fx.run()
        self.assertEqual(code, 1)
        self.assertIn("not a symbol name", err)

    def test_bad_label(self):
        (self.fx.curated / "a;b.txt").write_text("gamma\n")
        code, _, err = self.fx.run()
        self.assertEqual(code, 1)
        self.assertIn("source label", err)

    def test_one_module_per_nid_and_library(self):
        self.fx.row("lib", "import", "alpha_func", "libA", module="otherModule")
        code, _, err = self.fx.run()
        self.assertEqual(code, 1)
        self.assertIn("comes from modules otherModule and libA", err)  # lib.csv is read first

    def test_short_import_row(self):
        self.fx.run()
        (self.fx.imports / "lib.csv").write_text(",".join(IMPORT_FIELDS) + "\nimport,x\n")
        with mock.patch.object(m, "FILE_STEMS", ["lib"]):
            with self.assertRaisesRegex(m.MatchError, "lib.csv:2: wrong number of fields"):
                m.load_nids(self.fx.imports)

    def test_collision_is_an_error(self):
        (self.fx.curated / "hand.txt").write_text("gamma\nother\n")
        real = m.nid
        with mock.patch.object(m, "nid", lambda n: real("gamma") if n == "other" else real(n)):
            code, _, err = self.fx.run()
        self.assertEqual(code, 1)
        self.assertIn("both hash to", err)

    def test_elf_must_be_pinned(self):
        (self.fx.elf / "lib.elf").write_bytes(b"changed")
        pinned = {hashlib.sha256(b"original").hexdigest(): "lib.elf"}
        with mock.patch.object(m, "PINNED_ELFS", pinned):
            with self.assertRaisesRegex(m.MatchError, "not the pinned"):
                m.read_pinned_elfs(self.fx.elf)

    def test_missing_imports(self):
        with mock.patch.object(m, "FILE_STEMS", ["absent"]):
            with self.assertRaisesRegex(m.MatchError, "is missing"):
                m.load_nids(self.fx.imports)


@unittest.skipUnless(load_game_dir()
                     and all((m.IMPORTS_DIR / f"{stem}.csv").is_file() for stem in m.FILE_STEMS)
                     and all((m.ELF_DIR / name).is_file() for name in m.PINNED_ELFS.values()),
                     "needs BB_GAME_DIR and the analysis/ outputs")
class GameTest(unittest.TestCase):
    def test_libc_names(self):
        table = m.load_nids(m.IMPORTS_DIR)
        for name, h in KNOWN.items():
            self.assertIn("libc", table[h], name)
            self.assertTrue(table[h]["libc"].exporters, name)
        candidates: m.Candidates = {}
        elfs = m.read_pinned_elfs(m.ELF_DIR)
        m.read_strings(elfs, candidates)
        found = m.match(table, candidates)
        self.assertGreater(len(found), 100)

    def test_static_load_set(self):
        # The eboot and the two bundled PRXs it needs (docs/formats/imports.md).
        elfs = m.read_pinned_elfs(m.ELF_DIR)
        self.assertEqual(m.static_load_set(elfs), {"eboot", "libc", "libSceFios2"})


if __name__ == "__main__":
    unittest.main()
