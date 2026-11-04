#!/usr/bin/env python3
"""Tests for elfimports. Run: python3 -m unittest discover -s tools/elfimports

The synthetic tests need no game files. The game test runs only when BB_GAME_DIR is set
(environment or local.env). It rebuilds the ELFs from the SELFs in memory.
"""

from __future__ import annotations

import contextlib
import csv
import hashlib
import io
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS / "elfimports"))
sys.path.insert(0, str(TOOLS / "self2elf"))
sys.path.insert(0, str(TOOLS / "common"))
import elfimports as ei  # noqa: E402
import ps4elf as p  # noqa: E402
import self2elf  # noqa: E402
from elfbuilder import FUNC, GLOBAL, LOCAL, NOTYPE, OBJECT, SECTION, WEAK, Builder  # noqa: E402

NID_A = "AAAAAAAAAAA"
NID_B = "BBBBBBBBBBB"
NID_C = "CCCCCCCCCCC"


def game_like() -> tuple[bytes, bytes, bytes]:
    """An executable, a PRX it needs, and a PRX nobody needs."""
    exe = (Builder(module="game")
           .needed("libkernel.prx", 1, "libkernel")
           .needed("libBundled.prx", 2, "libBundled")
           .needed("libUnused.prx", 3, "libUnused")
           .import_lib(0, "libkernel").import_lib(1, "libPosix")
           .import_lib(2, "libBundled").import_lib(3, "libUnused")
           .sym("")  # unnamed section symbol, as in the real files
           .sym(f"{NID_A}#A#B").sym(f"{NID_B}#A#B", OBJECT).sym(f"{NID_C}#B#B")
           .sym(f"{NID_A}#C#C"))
    exe.symbols[0] = ("", SECTION, LOCAL, False)
    bundled = (Builder(prx=True, module="libBundled", soname="libBundled.prx")
               .needed("libkernel.prx", 1, "libkernel").import_lib(0, "libkernel")
               .export_lib(1, "libBundled")
               .sym(f"{NID_A}#A#B").sym(f"{NID_C}#A#B")
               .sym(f"{NID_A}#B#A", defined=True).sym(f"{NID_B}#B#A", OBJECT, WEAK, True)
               .sym("module_start", NOTYPE, WEAK))
    other = (Builder(prx=True, module="libOther", soname="libOther.prx")
             .needed("libkernel.prx", 1, "libkernel").import_lib(0, "libkernel")
             .export_lib(1, "libOther").sym(f"{NID_B}#A#B").sym(f"{NID_C}#B#A", defined=True))
    return exe.build(), bundled.build(), other.build()


def analyze(data: bytes, name: str = "x.elf") -> ei.ElfInfo:
    return ei.analyze(name, p.parse(data))


class AnalyzeTests(unittest.TestCase):
    def setUp(self):
        exe, bundled, _ = game_like()
        self.exe = analyze(exe, "game.elf")
        self.prx = analyze(bundled, "bundled.elf")

    def test_modules(self):
        self.assertEqual(self.exe.self_module.name, "game")
        self.assertEqual(self.exe.self_module.version, (1, 1))
        self.assertEqual({m.id: (m.name, m.needed) for m in self.exe.modules.values()},
                         {1: ("libkernel", "libkernel.prx"), 2: ("libBundled", "libBundled.prx"),
                          3: ("libUnused", "libUnused.prx")})

    def test_libraries(self):
        lib = self.exe.libraries[False, 1]
        self.assertEqual((lib.name, lib.version, lib.attr, lib.exported), ("libPosix", 1, 9, False))
        # A library is mapped to the module its symbols name: libPosix lives in libkernel.
        self.assertEqual(lib.module.name, "libkernel")
        self.assertIsNone(self.exe.libraries[False, 3].module)
        self.assertTrue(self.prx.libraries[True, 1].exported)

    def test_imports(self):
        got = [(e.nid, e.library.name, e.module.name, e.type) for e in self.exe.imports]
        self.assertEqual(got, [(NID_A, "libkernel", "libkernel", "func"),
                               (NID_B, "libkernel", "libkernel", "object"),
                               (NID_C, "libPosix", "libkernel", "func"),
                               (NID_A, "libBundled", "libBundled", "func")])
        self.assertEqual(self.exe.exports, [])
        self.assertEqual(self.exe.soname, None)

    def test_exports(self):
        got = [(e.nid, e.library.name, e.module.name, e.type, e.bind) for e in self.prx.exports]
        self.assertEqual(got, [(NID_A, "libBundled", "libBundled", "func", "global"),
                               (NID_B, "libBundled", "libBundled", "object", "weak")])
        self.assertTrue(all(e.value for e in self.prx.exports))
        self.assertEqual(self.prx.plain_symbols, ["module_start"])
        self.assertEqual(self.prx.soname, "libBundled.prx")


class RejectTests(unittest.TestCase):
    def assertRejects(self, builder: Builder, message: str):
        with self.assertRaisesRegex(p.ElfError, message):
            analyze(builder.build())

    def base(self) -> Builder:
        return Builder().needed("k.prx", 1, "k").import_lib(0, "k")

    def test_unknown_library(self):
        self.assertRejects(self.base().sym(f"{NID_A}#B#B"), "unknown library")

    def test_unknown_module(self):
        self.assertRejects(self.base().sym(f"{NID_A}#A#C"), "unknown module")

    def test_import_from_own_module(self):
        self.assertRejects(self.base().sym(f"{NID_A}#A#A"), "unknown module")

    def test_multi_digit_id_refused(self):
        self.assertRejects(self.base().sym(f"{NID_A}#AB#B"), "unsupported id")

    def test_malformed_nid(self):
        self.assertRejects(self.base().sym("short#A#B"), "malformed NID")

    def test_malformed_name(self):
        self.assertRejects(self.base().sym(f"{NID_A}#A"), "malformed symbol name")

    def test_needed_numbering_differs(self):
        self.assertRejects(Builder().needed("a.prx", 2, "a"), "not numbered 1..n")

    def test_plain_import_refused(self):
        self.assertRejects(self.base().sym("memcpy"), "without a NID")

    def test_plain_module_start_must_be_weak_reference(self):
        self.assertRejects(self.base().sym("module_start", FUNC, GLOBAL, True), "without a NID")

    def test_unnamed_non_section_symbol(self):
        self.assertRejects(self.base().sym(""), "malformed symbol name")

    def test_library_in_two_modules(self):
        b = self.base().needed("j.prx", 2, "j").sym(f"{NID_A}#A#B").sym(f"{NID_B}#A#C")
        self.assertRejects(b, "two modules")

    def test_duplicate_module(self):
        self.assertRejects(self.base().record(p.DT_SCE_NEEDED_MODULE, 1, 0x0101, "x"),
                           "duplicate module")

    def test_duplicate_library(self):
        self.assertRejects(self.base().record(p.DT_SCE_IMPORT_LIB, 0, 1, "x"), "duplicate library")

    def test_attribute_for_unknown_library(self):
        b = self.base()
        b.entries.append((p.DT_SCE_IMPORT_LIB_ATTR, 5 << 48 | 9))
        self.assertRejects(b, "unknown library 5")

    def test_unexpected_attribute_bits(self):
        b = self.base()
        b.entries.append((p.DT_SCE_IMPORT_LIB_ATTR, 1 << 32 | 9))
        self.assertRejects(b, "unexpected bits")


class CrossFileTests(unittest.TestCase):
    def setUp(self):
        exe, bundled, other = game_like()
        self.infos = [analyze(exe, "game.elf"), analyze(bundled, "bundled.elf"),
                      analyze(other, "other.elf")]
        self.text = ei.summary(self.infos)

    def test_providers(self):
        self.assertEqual(ei.providers([i for i in self.infos if i.is_prx]),
                         {("libBundled", "libBundled", NID_A): "bundled.elf",
                          ("libBundled", "libBundled", NID_B): "bundled.elf",
                          ("libOther", "libOther", NID_C): "other.elf"})

    def test_resolvers_only_use_loaded_prxs(self):
        resolved = ei.resolvers(self.infos)
        self.assertEqual(set(resolved["game.elf"].values()), {"bundled.elf"})
        self.assertEqual(resolved["other.elf"], {})

    def test_duplicate_export_refused(self):
        twin = Builder(prx=True, module="libBundled", soname="twin.prx").export_lib(1, "libBundled")
        twin.sym(f"{NID_C}#B#A", defined=True)
        with self.assertRaisesRegex(p.ElfError, "exported by bundled.elf and twin.elf"):
            ei.providers([self.infos[1], analyze(twin.build(), "twin.elf")])

    def test_static_load_set(self):
        self.assertEqual([i.name for i in ei.static_load_set(self.infos)],
                         ["game.elf", "bundled.elf"])

    def test_firmware_scope(self):
        # libkernel: A and B from the game, A and C from the bundled PRX -> 3 NIDs.
        self.assertIn("| libkernel | libkernel | 3 | 2 func, 1 object | bundled.elf, game.elf |",
                      self.text)
        self.assertIn("| libPosix | libkernel | 1 | 1 func | game.elf |", self.text)
        self.assertIn("4 NIDs in 2 libraries.", self.text)
        self.assertIn("| libBundled | bundled.elf | 1 | 2 |", self.text)
        self.assertIn("- game.elf: libUnused", self.text)
        self.assertIn("| other.elf | libkernel (1) |", self.text)

    def test_unresolved_bundled_import(self):
        exe = (Builder().needed("libBundled.prx", 1, "libBundled").import_lib(0, "libBundled")
               .sym(f"{NID_C}#A#B").build())
        text = ei.summary([analyze(exe, "game.elf"), self.infos[1]])
        self.assertIn("- game.elf: 1 from libBundled", text)

    def test_export_of_unloaded_prx_is_not_bundled(self):
        # The game imports from module libBundled, but its DT_NEEDED names another file, so
        # bundled.elf is not loaded and can't provide the import.
        exe = (Builder().needed("libBundled_x.prx", 1, "libBundled").import_lib(0, "libBundled")
               .sym(f"{NID_A}#A#B").build())
        text = ei.summary([analyze(exe, "game.elf"), self.infos[1]])
        self.assertIn("| libBundled | libBundled | 1 | 1 func | game.elf |", text)
        self.assertIn("- game.elf: 1 from libBundled", text)
        self.assertIn("|---|---|---|---|\n\n## Unresolved", text)  # no bundled use

    def test_same_library_from_two_modules(self):
        exe = (Builder().needed("a.prx", 1, "modA").needed("b.prx", 2, "modB")
               .import_lib(0, "libX").import_lib(1, "libX")
               .sym(f"{NID_A}#A#B").sym(f"{NID_B}#B#C").build())
        text = ei.summary([analyze(exe, "game.elf")])
        self.assertIn("| libX | modA | 1 | 1 func | game.elf |", text)
        self.assertIn("| libX | modB | 1 | 1 func | game.elf |", text)

    def test_nid_with_two_types_counts_once(self):
        # The same libkernel-style NID, imported as func by the game and as object by a PRX.
        prx =(Builder(prx=True, module="p", soname="p.prx").needed("k.prx", 1, "k")
               .import_lib(0, "k").sym(f"{NID_A}#A#B", OBJECT).build())
        exe = (Builder().needed("k.prx", 1, "k").needed("p.prx", 2, "p").import_lib(0, "k")
               .sym(f"{NID_A}#A#B").build())
        text = ei.summary([analyze(exe, "game.elf"), analyze(prx, "p.elf")])
        self.assertIn("| k | k | 1 | 1 mixed | game.elf, p.elf |", text)

    def test_summary_has_no_nids(self):
        for nid in (NID_A, NID_B, NID_C):
            self.assertNotIn(nid, self.text)


class MainTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)
        self.elf_dir = self.tmp / "elf"
        self.elf_dir.mkdir()
        names = ["game.elf", "bundled.elf", "other.elf"]
        self.files = []
        known = {}
        for name, data in zip(names, game_like()):
            path = self.elf_dir / name
            path.write_bytes(data)
            self.files.append(path)
            known[hashlib.sha256(data).hexdigest()] = name
        for patch in (mock.patch.object(ei, "KNOWN_FILES", known),
                      mock.patch.object(ei, "DEFAULT_IN", self.elf_dir)):
            patch.start()
            self.addCleanup(patch.stop)

    def run_main(self, *argv: str) -> tuple[int, str]:
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr), contextlib.redirect_stdout(io.StringIO()):
            status = ei.main(list(argv))
        return status, stderr.getvalue()

    def test_default_run_writes_outputs(self):
        out = self.tmp / "out"
        self.assertEqual(self.run_main("-o", str(out)), (0, ""))
        self.assertEqual(sorted(f.name for f in out.iterdir()),
                         ["bundled.csv", "game.csv", "other.csv", "summary.md"])
        with (out / "game.csv").open() as fh:
            rows = list(csv.DictReader(fh))
        self.assertEqual([(r["nid"], r["library"], r["provided_by"]) for r in rows],
                         [(NID_A, "libkernel", "firmware"), (NID_B, "libkernel", "firmware"),
                          (NID_C, "libPosix", "firmware"), (NID_A, "libBundled", "bundled.elf")])
        with (out / "bundled.csv").open() as fh:
            exports = [r for r in csv.DictReader(fh) if r["direction"] == "export"]
        self.assertEqual([r["value"] for r in exports], ["0x1030", "0x1040"])

    def test_outputs_of_earlier_runs_removed(self):
        out = self.tmp / "out"
        self.run_main("-o", str(out))
        (out / "notes.txt").write_text("not ours")
        self.assertEqual(self.run_main(str(self.files[0]), "-o", str(out)), (0, ""))
        self.assertEqual(sorted(f.name for f in out.iterdir()),
                         ["game.csv", "notes.txt", "summary.md"])

    def test_unpinned_file_refused_and_nothing_written(self):
        stray = self.tmp / "in" / "stray.elf"
        stray.parent.mkdir()
        stray.write_bytes(Builder(module="stray").build())
        out = self.tmp / "out"
        status, err = self.run_main(str(self.files[0]), str(stray), "-o", str(out))
        self.assertEqual(status, 1)
        self.assertIn("not a pinned", err)
        self.assertFalse(out.exists())

    def test_same_file_twice_refused(self):
        out = self.tmp / "out"
        status, err = self.run_main(str(self.files[0]), str(self.files[0]), "-o", str(out))
        self.assertEqual(status, 1)
        self.assertIn("twice", err)

    def test_output_into_input_dir_refused(self):
        self.assertEqual(self.run_main("-o", str(self.elf_dir / "sub"))[0], 2)
        self.assertFalse((self.elf_dir / "sub").exists())


GAME_DIR = ei.load_game_dir()


@unittest.skipUnless(GAME_DIR, "BB_GAME_DIR not set (environment or local.env)")
class GameTests(unittest.TestCase):
    def test_all_pinned_elfs(self):
        infos = []
        for path in self2elf.default_inputs(GAME_DIR):
            data = self2elf.self_to_elf(path.read_bytes()).data
            known = ei.KNOWN_FILES.get(hashlib.sha256(data).hexdigest())
            self.assertIsNotNone(known, path.name)
            infos.append(ei.analyze(known, p.parse(data)))
        self.assertEqual({i.name for i in infos}, set(ei.KNOWN_FILES.values()))
        text = ei.summary(infos)
        self.assertIn("## Unresolved imports from bundled libraries\n\nNone.", text)
        self.assertEqual([i.name for i in ei.static_load_set(infos)],
                         ["eboot.elf", "libSceFios2.elf", "libc.elf"])
        # Plain substring search: NIDs may start or end with + or -, which word boundaries miss.
        nids = {e.nid for i in infos for e in i.imports + i.exports}
        self.assertEqual([n for n in nids if n in text], [], "summary contains NIDs")


if __name__ == "__main__":
    unittest.main()
