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
import re
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS / "elfimports"))
sys.path.insert(0, str(TOOLS / "self2elf"))
import elfimports as ei  # noqa: E402
import ps4elf as p  # noqa: E402
import self2elf  # noqa: E402

FUNC, OBJECT, SECTION = 2, 1, 3
GLOBAL, WEAK, LOCAL = 1, 2, 0

NID_A = "AAAAAAAAAAA"
NID_B = "BBBBBBBBBBB"
NID_C = "CCCCCCCCCCC"


class Builder:
    """A minimal PS4 ELF: one dynlibdata segment, a dynamic table, records and symbols."""

    def __init__(self, prx: bool = False, module: str = "main", soname: str | None = None):
        self.prx = prx
        self.strtab = bytearray(b"\0")
        self.entries: list[tuple[int, int]] = []
        self.symbols: list[tuple[str, int, int, bool]] = []
        if soname:
            self.entries.append((p.DT_SONAME, self.str(soname)))
        self.record(p.DT_SCE_MODULE_INFO, 0, 0x0101, module)

    def str(self, text: str) -> int:
        off = len(self.strtab)
        self.strtab += text.encode() + b"\0"
        return off

    def record(self, tag: int, ident: int, version: int, name: str) -> Builder:
        self.entries.append((tag, ident << 48 | version << 32 | self.str(name)))
        return self

    def needed(self, file_name: str, mod_id: int, name: str) -> Builder:
        self.entries.append((p.DT_NEEDED, self.str(file_name)))
        return self.record(p.DT_SCE_NEEDED_MODULE, mod_id, 0x0101, name)

    def import_lib(self, lib_id: int, name: str, attr: int = 9) -> Builder:
        self.record(p.DT_SCE_IMPORT_LIB, lib_id, 1, name)
        self.entries.append((p.DT_SCE_IMPORT_LIB_ATTR, lib_id << 48 | attr))
        return self

    def export_lib(self, lib_id: int, name: str) -> Builder:
        self.record(p.DT_SCE_EXPORT_LIB, lib_id, 1, name)
        self.entries.append((p.DT_SCE_EXPORT_LIB_ATTR, lib_id << 48 | 1))
        return self

    def sym(self, name: str, typ: int = FUNC, bind: int = GLOBAL, defined: bool = False) -> Builder:
        self.symbols.append((name, typ, bind, defined))
        return self

    def build(self) -> bytes:
        syms = [p.SYM.pack(0, 0, 0, 0, 0, 0)]
        for index, (name, typ, bind, defined) in enumerate(self.symbols, start=1):
            name_off = self.str(name) if name else 0
            syms.append(p.SYM.pack(name_off, bind << 4 | typ, 0, 1 if defined else 0,
                                   0x1000 + index * 0x10 if defined else 0, 0))
        symtab = b"".join(syms)
        blob = bytearray(bytes(0x10))
        str_off = len(blob)
        blob += self.strtab
        while len(blob) % 8:
            blob += b"\0"
        sym_off = len(blob)
        blob += symtab
        tables = {
            p.DT_SCE_PLTGOT: 0, p.DT_SCE_JMPREL: sym_off, p.DT_SCE_PLTREL: p.DT_RELA,
            p.DT_SCE_PLTRELSZ: 0, p.DT_SCE_RELA: sym_off, p.DT_SCE_RELASZ: 0,
            p.DT_SCE_RELAENT: p.RELA_SIZE, p.DT_SCE_STRTAB: str_off,
            p.DT_SCE_STRSZ: len(self.strtab), p.DT_SCE_SYMTAB: sym_off,
            p.DT_SCE_SYMENT: p.SYM.size, p.DT_SCE_SYMTABSZ: len(symtab),
        }
        dyn_off = len(blob)
        for tag, value in [*self.entries, *tables.items(), (p.DT_NULL, 0)]:
            blob += p.DYN.pack(tag, value)

        phoff, dynlib_off = 0x40, 0x1000
        phdrs = [
            p.Phdr(p.PT_LOAD, 5, 0, 0, 0, 0x100, 0x100, 0x4000),
            p.Phdr(p.PT_DYNAMIC, 6, dynlib_off + dyn_off, 0, 0, len(blob) - dyn_off,
                   len(blob) - dyn_off, 8),
            p.Phdr(p.PT_SCE_DYNLIBDATA, 4, dynlib_off, 0, 0, len(blob), 0, 0x10),
        ]
        out = bytearray(dynlib_off + len(blob))
        e_type = p.ET_SCE_DYNAMIC if self.prx else p.ET_SCE_DYNEXEC
        ident = b"\x7fELF\x02\x01\x01\x09" + bytes(8)
        out[:0x40] = p.ELF_HEADER.pack(ident, e_type, p.EM_X86_64, 1, 0, phoff, 0, 0, 0x40,
                                       p.ELF_PHDR.size, len(phdrs), 0x40, 0, 0)
        for i, ph in enumerate(phdrs):
            out[phoff + i * p.ELF_PHDR.size: phoff + (i + 1) * p.ELF_PHDR.size] = ph.pack()
        out[dynlib_off:] = blob
        return bytes(out)


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
               .sym("module_start", defined=True))
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

    def test_needed_not_paired_when_numbering_differs(self):
        data = Builder().needed("a.prx", 2, "a").build()
        self.assertIsNone(analyze(data).modules[2].needed)


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
        self.provided = ei.providers([i for i in self.infos if i.is_prx])
        self.text = ei.summary(self.infos, self.provided)

    def test_providers(self):
        self.assertEqual(self.provided, {("libBundled", NID_A): "bundled.elf",
                                         ("libBundled", NID_B): "bundled.elf",
                                         ("libOther", NID_C): "other.elf"})

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
        infos = [analyze(exe, "game.elf"), self.infos[1]]
        text = ei.summary(infos, ei.providers(infos[1:]))
        self.assertIn("- game.elf: 1 from libBundled", text)

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
NID_PATTERN = re.compile(r"\b[A-Za-z0-9+-]{11}\b")


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
        provided = ei.providers([i for i in infos if i.is_prx])
        text = ei.summary(infos, provided)
        self.assertIn("## Unresolved imports from bundled libraries\n\nNone.", text)
        self.assertEqual([i.name for i in ei.static_load_set(infos)],
                         ["eboot.elf", "libSceFios2.elf", "libc.elf"])
        nids = {e.nid for i in infos for e in i.imports + i.exports}
        self.assertFalse(nids & set(NID_PATTERN.findall(text)), "summary contains NIDs")


if __name__ == "__main__":
    unittest.main()
