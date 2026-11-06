#!/usr/bin/env python3
"""Tests for nid and nidmatch. Run: python3 -m unittest discover -s tools/nidmatch

The synthetic tests need no game files. The game test runs only when BB_GAME_DIR is set and the
outputs of self2elf exist in analysis/elf/.
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
sys.path.insert(0, str(TOOLS / "elfimports"))
import derive  # noqa: E402
import elfimports  # noqa: E402
import freebsdman  # noqa: E402
import nidmatch as m  # noqa: E402
import syscandidates  # noqa: E402
from cli import pinned_elf_paths  # noqa: E402
from elfbuilder import Builder  # noqa: E402
from nid import NID_ALPHABET, NID_SUFFIX, is_nid, nid, nid_digest  # noqa: E402
from toolenv import load_game_dir  # noqa: E402

# Exports of the pinned libc.prx, named by matching. strlen (0x2f970) and strcmp (0x30160) were
# also checked against their disassembly. Derived by us, see docs/formats/nid.md.
KNOWN = {
    "strlen": "j4ViWNHEgww",
    "strcmp": "Ovb2dSJOAuE",
    "memset": "8zTFvBIAIN8",
}


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
    """A fake analysis/ and symbols/ tree with three pinned files: the executable 'main', the
    PRX 'lib' that main needs, and the PRX 'other' that nothing needs. So the load set is main
    and lib.

    imp() and exp() add symbols. Each file imports a module M through DT_NEEDED M.prx unless
    imp() names another file. So main gets the exports of module 'lib' from lib.elf, and
    everything else comes from the firmware.
    """

    PRXS = {"lib", "other"}

    def __init__(self, root: Path):
        self.root = root
        self.elf = root / "elf"
        self.out = root / "nids"
        self.curated = root / "curated"
        self.nids_csv = root / "nids.csv"
        for d in (self.elf, self.curated):
            d.mkdir()
        self.stems = ["main", *sorted(self.PRXS)]
        # stem -> [(direction, name, library, module)]
        self.syms: dict[str, list[tuple[str, str, str, str]]] = {stem: [] for stem in self.stems}
        self.needed: dict[tuple[str, str], str] = {}  # (stem, module) -> DT_NEEDED file name
        self.strings = {"main": ["alpha_func"]}  # identifiers in the file, not symbols
        self.tampered: set[str] = set()  # stems written with an extra byte after pinning

    def imp(self, stem: str, name: str, library: str, module: str | None = None,
            needed: str | None = None) -> Fixture:
        module = module or library
        self.syms[stem].append(("import", name, library, module))
        file_name = needed or f"{module}.prx"
        # A file has one DT_NEEDED entry per module, so all imports of a module share it.
        if self.needed.setdefault((stem, module), file_name) != file_name:
            raise ValueError(f"{stem} already needs {self.needed[stem, module]} for {module}")
        return self

    def exp(self, stem: str, name: str, library: str) -> Fixture:
        self.syms[stem].append(("export", name, library, stem))
        return self

    def build(self, stem: str) -> bytes:
        prx = stem in self.PRXS
        b = Builder(prx=prx, module=stem, soname=f"{stem}.prx" if prx else None)
        for text in self.strings.get(stem, []):
            b.str(text)
        syms = self.syms[stem]
        modules = {stem: 0}
        for direction, _, _, module in syms:
            if direction == "import" and module not in modules:
                modules[module] = len(modules)
                b.needed(self.needed[stem, module], modules[module], module)
        libraries: dict[tuple[str, str], int] = {}
        for direction, _, library, _ in syms:
            if (direction, library) not in libraries:
                libraries[direction, library] = len(libraries)
                lib_id = libraries[direction, library]
                (b.import_lib if direction == "import" else b.export_lib)(lib_id, library)
        for direction, name, library, module in syms:
            lib_id, mod_id = libraries[direction, library], modules[module]
            b.sym(f"{nid(name)}#{NID_ALPHABET[lib_id]}#{NID_ALPHABET[mod_id]}",
                  defined=direction == "export")
        return b.build()

    def run(self, *args: str) -> tuple[int, str, str]:
        pinned = {}
        for stem in self.stems:
            data = self.build(stem)
            pinned[hashlib.sha256(data).hexdigest()] = f"{stem}.elf"
            (self.elf / f"{stem}.elf").write_bytes(data + b"x" * (stem in self.tampered))
        out, err = io.StringIO(), io.StringIO()
        with contextlib.ExitStack() as stack:
            for attr, value in [("ELF_DIR", self.elf), ("OUT_DIR", self.out),
                                ("GENERATED_DIR", self.out / "candidates"),
                                ("NIDS_CSV", self.nids_csv), ("CURATED_DIR", self.curated)]:
                stack.enter_context(mock.patch.object(m, attr, value))
            stack.enter_context(mock.patch.object(elfimports, "PINNED_ELFS", pinned))
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
        (self.fx.imp("main", "alpha_func", "libA")
                .imp("main", "gamma", "libB", module="lib")
                .imp("main", "unknown_one", "libA")
                .exp("lib", "gamma", "libB"))

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

    def test_generated_labels_are_rederived(self):
        generated = self.fx.out / "candidates"
        generated.mkdir(parents=True)
        (generated / "derive-words.txt").write_text("gamma\n")
        self.fx.nids_csv.write_text("nid,library,name,source\n"
                                    f"{nid('unknown_one')},libA,unknown_one,derive-words\n")
        self.assertEqual(self.fx.run()[0], 0)
        # The list no longer holds unknown_one, so its earlier match is dropped.
        self.assertEqual({r["name"]: r["source"] for r in self.fx.nids()},
                         {"alpha_func": "strings:main", "gamma": "derive-words"})
        # Without the list, its names are kept.
        (generated / "derive-words.txt").unlink()
        self.assertEqual(self.fx.run()[0], 0)
        self.assertIn("gamma", [r["name"] for r in self.fx.nids()])

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
            (f"{nid('alpha_func')},libA,stré,x\n", "not a symbol name"),
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

    def test_duplicate_label(self):
        (self.fx.curated / "hand.txt").write_text("gamma\n")
        extra = self.fx.root / "hand.txt"
        extra.write_text("unknown_one\n")
        code, _, err = self.fx.run(str(extra))
        self.assertEqual(code, 1)
        self.assertIn("both give the source label 'hand'", err)

    def test_firmware_count_is_per_importer(self):
        # main gets delta from lib.elf. other, outside the load set, needs lib.sprx instead and so
        # gets delta from the firmware. So delta is not a firmware import of the load set.
        (self.fx.imp("main", "delta", "libC", module="lib")
                .exp("lib", "delta", "libC")
                .imp("other", "delta", "libC", module="lib", needed="lib.sprx"))
        code, out, err = self.fx.run()
        self.assertEqual(code, 0, err)
        self.assertIn("static load set: 1 of 2", out)

    def test_one_module_per_nid_and_library(self):
        self.fx.imp("lib", "alpha_func", "libA", module="otherModule")
        code, _, err = self.fx.run()
        self.assertEqual(code, 1)
        self.assertIn("comes from modules otherModule and libA", err)  # lib.elf is read first

    def test_collision_is_an_error(self):
        (self.fx.curated / "hand.txt").write_text("gamma\nother\n")
        real = m.nid
        with mock.patch.object(m, "nid", lambda n: real("gamma") if n == "other" else real(n)):
            code, _, err = self.fx.run()
        self.assertEqual(code, 1)
        self.assertIn("both hash to", err)

    def test_elf_must_be_pinned(self):
        # A file changed after self2elf wrote it is refused, with a hint to rebuild it.
        self.fx.tampered.add("lib")
        code, _, err = self.fx.run()
        self.assertEqual(code, 1)
        self.assertIn("lib.elf: error: not a pinned file", err)
        self.assertIn("run tools/self2elf again", err)

    def test_elf_must_have_its_pinned_name(self):
        # Pinned contents under the name of another pinned file.
        for name, data in [("main.elf", b"one"), ("lib.elf", b"one")]:
            (self.fx.elf / name).write_bytes(data)
        pinned = {hashlib.sha256(b"one").hexdigest(): "main.elf",
                  hashlib.sha256(b"two").hexdigest(): "lib.elf"}
        with mock.patch.object(elfimports, "PINNED_ELFS", pinned):
            with self.assertRaisesRegex(elfimports.LoadError, "lib.elf: error: holds the pinned "
                                                              "main.elf"):
                elfimports.load_dir(self.fx.elf)

    def test_needed_conflict_in_fixture(self):
        self.fx.imp("other", "x", "libC", module="lib")
        with self.assertRaises(ValueError):
            self.fx.imp("other", "delta", "libC", module="lib", needed="lib.sprx")

    def test_public_list_label(self):
        # A list's file stem names the list and its revision, and is the label of its matches.
        self.fx.imp("main", "sceRtcThing", "libSceRtc")
        listed = self.fx.root / "shadps4-0123abc.txt"
        listed.write_text("# names of a public NID list\nsceRtcThing\nnot_a_match\n")
        code, _, err = self.fx.run(str(listed))
        self.assertEqual(code, 0, err)
        row = next(r for r in self.fx.nids() if r["name"] == "sceRtcThing")
        self.assertEqual(row["source"], "shadps4-0123abc")
        # Without the list, the label is carried over.
        listed.unlink()
        self.assertEqual(self.fx.run()[0], 0)
        self.assertIn("sceRtcThing", [r["name"] for r in self.fx.nids()])

    def test_derived_name_of_formerly_excluded_library(self):
        # The exposure log no longer restricts derived names.
        self.fx.imp("main", "sceRtcThing", "libSceRtc")
        derived = self.fx.root / "derive-words.txt"
        derived.write_text("sceRtcThing\n")
        code, _, err = self.fx.run(str(derived))
        self.assertEqual(code, 0, err)
        row = next(r for r in self.fx.nids() if r["name"] == "sceRtcThing")
        self.assertEqual(row["source"], "derive-words")


def table_of(names_by_lib: dict[str, list[str]]) -> m.NidTable:
    return {nid(n): {lib: m.Use(lib)} for lib, names in names_by_lib.items() for n in names}


class DeriveTest(unittest.TestCase):
    def test_nid_digest(self):
        for name in ["strlen", "sceFoo"]:
            digest = hashlib.sha1(name.encode() + NID_SUFFIX).digest()
            self.assertEqual(nid_digest(nid(name)), digest[:8])

    def test_words(self):
        self.assertEqual(derive.words("FiosFHOpenSync"), ["Fios", "FH", "Open", "Sync"])
        self.assertEqual(derive.words("pthread_attr_setstacksize"),
                         ["Pthread", "Attr", "Setstacksize"])
        self.assertEqual(derive.camel("pthread_attr_setstacksize"), "PthreadAttrSetstacksize")
        self.assertEqual(derive.camel("_exit"), "Exit")

    def test_prefixes(self):
        self.assertEqual(derive.prefixes("libSceNpMatching2"),
                         {"sceNp", "sceNpMatching", "sceNpMatching2"})
        self.assertEqual(derive.prefixes("libSceFios2"), {"sceFios", "sceFios2"})
        self.assertEqual(derive.prefixes("libkernel"), {"sce"})
        self.assertEqual(derive.prefixes("libSceNet"), {"sceNet"})
        self.assertEqual(derive.prefixes("libc"), set())
        self.assertEqual(derive.prefixes("libSceRtc"), {"sceRtc"})
        self.assertEqual(derive.prefixes("libSceSaveDataDialog"),
                         {"sceSave", "sceSaveData", "sceSaveDataDialog"})

    def test_new_combos_partition(self):
        old, new = [b"a", b"b"], [b"c", b"d", b"e"]
        for k in (1, 2, 3):
            combos = list(derive.new_combos(old, new, k))
            self.assertEqual(len(combos), len(set(combos)))
            self.assertEqual(len(combos), 5 ** k - 2 ** k)
            self.assertTrue(all(any(w in new for w in c) for c in combos))
        self.assertEqual(list(derive.new_combos([], [b"x"], 2)), [(b"x", b"x")])

    def test_derive(self):
        table = table_of({
            "libkernel": ["sceKernelOpen",            # no match motivates a sceKernel prefix
                          "scePthreadSelf", "sceExit"],
            "libSceFoo": ["sceFooBarOpen",          # sce words: Bar from the seed, Open
                          "sceFooQuuxZap",          # all words: Quux, Zap from identifiers
                          "sceFooBarOpenQuuxZap",   # 4 words: out of reach
                          "sceKernelOpenThing"],    # wrong prefix for libSceFoo
            "libSceRtc": ["sceRtcOpen"],            # no longer excluded
        })
        seed = {"sceFooBarClose"}
        c_names = {"open", "pthread_self", "_exit", "Not_C"}
        idents = {"QuuxZap", "useQuux", "anotherZap", "lonelyWord"}
        result = derive.derive(derive.targets(table), seed, c_names, idents)
        self.assertEqual(result[derive.POSIX_LABEL], {"scePthreadSelf", "sceExit", "sceRtcOpen"})
        self.assertEqual(result[derive.WORDS_LABEL],
                         {"scePthreadSelf", "sceExit", "sceFooBarOpen", "sceFooQuuxZap",
                          "sceRtcOpen"})
        # A pool of workers finds the same names.
        self.assertEqual(derive.derive(derive.targets(table), seed, c_names, idents, jobs=2),
                         result)

    def test_read_seed(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "nids.csv"
            path.write_text("nid,library,name,source\n"
                            "x,libA,a,derive-words\n"
                            "x,libA,b,derive-posix;glibc\n"
                            "x,libA,c,strings:eboot\n"
                            "x,libSceRtc,d,hand\n"
                            "x,libA,e,shadps4-0123abc\n")
            self.assertEqual(derive.read_seed(path), {"b", "c", "d", "e"})
            self.assertEqual(derive.read_seed(path, frozenset({"shadps4-0123abc", "hand"})),
                             {"b", "c"})
            path.write_text("nid,library,name,source\nx,libA,a\n")
            with self.assertRaisesRegex(m.MatchError, "wrong number of fields"):
                derive.read_seed(path)
            path.write_text("nid,lib,name,source\nx,libA,a,glibc\n")
            with self.assertRaisesRegex(m.MatchError, "expected columns"):
                derive.read_seed(path)


class ManNamesTest(unittest.TestCase):
    def test_page_names(self):
        with tempfile.TemporaryDirectory() as tmp:
            man3 = Path(tmp) / "man3"
            man3.mkdir()
            for name in ["pthread_create.3.gz", "BIO_read.3ssl.gz", "qsort.3", "README",
                         "index.db", "foo.3bar.3.gz", "printf.1.gz"]:
                (man3 / name).touch()
            self.assertEqual(syscandidates.man_names([Path(tmp), Path(tmp) / "absent"], "3"),
                             {"pthread_create", "BIO_read", "qsort"})


class FreebsdIndexTest(unittest.TestCase):
    PAGE = """<dl>
<dt><a href="/cgi/man.cgi?query=access&amp;sektion=2&amp;apropos=0&amp;manpath=FreeBSD+10.4-RELEASE">access(2), eaccess(2), faccessat(2)</a> - check access</dt>
<dt><a href="/cgi/man.cgi?query=au_bsm_to_fcntl_cmd&amp;sektion=3&amp;apropos=0&amp;manpath=FreeBSD+10.4-RELEASE">au_bsm_to_fcntl_cmd(3), au_fcntl_cmd_to_bsm(3)</a></dt>
<dt><a href="/cgi/man.cgi?query=_exit&amp;sektion=2&amp;apropos=0&amp;manpath=FreeBSD+10.4-RELEASE"><b>_exit</b>(2)</a></dt>
<a href="/cgi/man.cgi?manpath=FreeBSD+10.4-RELEASE">home</a>
</dl>"""

    def test_names_of_one_section(self):
        # Every name a link documents, but only those of the section asked for: the section 2
        # result page also links section 3 pages.
        self.assertEqual(freebsdman.index_names(self.PAGE, "2"),
                         {"access", "eaccess", "faccessat", "_exit"})
        self.assertEqual(freebsdman.index_names(self.PAGE, "3"),
                         {"au_bsm_to_fcntl_cmd", "au_fcntl_cmd_to_bsm"})


@unittest.skipUnless(load_game_dir()
                     and all(path.is_file()
                             for path in pinned_elf_paths(m.ELF_DIR, elfimports.PINNED_ELFS)),
                     "needs BB_GAME_DIR and the self2elf outputs in analysis/elf/")
class GameTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.infos, cls.elfs = elfimports.load_dir(m.ELF_DIR)

    def test_libc_names(self):
        table = m.load_nids(self.infos)
        for name, h in KNOWN.items():
            self.assertIn("libc", table[h], name)
            self.assertTrue(table[h]["libc"].exporters, name)
        candidates: m.Candidates = {}
        m.read_strings(self.elfs, candidates)
        found = m.match(table, candidates)
        self.assertGreater(len(found), 100)

    def test_static_load_set(self):
        # The eboot and the two bundled PRXs it needs (docs/formats/imports.md).
        self.assertEqual([i.name for i in elfimports.static_load_set(self.infos)],
                         ["eboot.elf", "libSceFios2.elf", "libc.elf"])


if __name__ == "__main__":
    unittest.main()
