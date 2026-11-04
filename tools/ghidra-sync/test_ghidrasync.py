#!/usr/bin/env python3
"""Tests for ghidrasync. Run: python3 -m unittest discover -s tools/ghidra-sync

The CSV tests need neither Ghidra nor the game. The round trip runs headless Ghidra on a small
ELF built here and skips when GHIDRA_INSTALL_DIR is unset (environment or local.env).
"""

from __future__ import annotations

import contextlib
import io
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ghidrasync as g  # noqa: E402

HEADER = "address,name,signature,status,file\n"


def table(text: str, tmp: Path) -> dict:
    path = tmp / "functions.csv"
    path.write_text(HEADER + text)
    return g.read_functions(path)


class FunctionsCsvTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(self.enterContext(tempfile.TemporaryDirectory()))

    def test_format_sorts_and_quotes(self):
        rows = table("0x20,b,,named,\n0x00000010,a,\"int a(int x, int y)\",replaced,src/a.cpp\n",
                     self.tmp)
        self.assertEqual(g.format_functions(rows), HEADER
                         + "0x00000010,a,\"int a(int x, int y)\",replaced,src/a.cpp\n"
                         + "0x00000020,b,,named,\n")

    def test_merge_keeps_status_and_file(self):
        rows = table("0x00000010,old,,decompiled,src/a.cpp\n0x00000030,keep,int keep(void),"
                     "replaced,src/k.cpp\n", self.tmp)
        merged = g.merge_functions(rows, [
            {"address": 0x10, "name": "new", "signature": "int new(void)"},
            {"address": 0x20, "name": "added", "signature": ""},
            {"address": 0x40, "name": "", "signature": "void FUN_00000040(void)"},
            {"address": 0x30, "name": "", "signature": ""},
        ])
        self.assertEqual(g.format_functions(merged), HEADER
                         + "0x00000010,new,int new(void),decompiled,src/a.cpp\n"
                         + "0x00000020,added,,named,\n"
                         + "0x00000030,keep,int keep(void),replaced,src/k.cpp\n"
                         + "0x00000040,,void FUN_00000040(void),unknown,\n")

    def test_merge_of_own_import_is_identity(self):
        rows = table("0x00000010,a,int a(void),named,\n0x00000020,,,unknown,\n", self.tmp)
        merged = g.merge_functions(rows, g.import_rows(rows))
        self.assertEqual(merged, rows)
        self.assertEqual([r["address"] for r in g.import_rows(rows)], [0x10])

    def test_missing_file_is_empty(self):
        self.assertEqual(g.read_functions(self.tmp / "none.csv"), {})

    def test_rejects_bad_input(self):
        for text in ("0x10,a,,named,\n0x010,b,,named,\n", "zz,a,,named,\n"):
            with self.subTest(text=text), self.assertRaises(g.SyncError):
                table(text, self.tmp)
        (self.tmp / "functions.csv").write_text("address,name\n")
        with self.assertRaises(g.SyncError):
            g.read_functions(self.tmp / "functions.csv")

    def test_headless_command(self):
        cmd = g.headless_command(Path("/g"), Path("/p/proj.gpr"), ["-process", "x"],
                                 [["A.py", "1"], ["B.py"]])
        self.assertEqual(cmd[-11:], ["/p", "proj", "-process", "x", "-scriptPath",
                                     str(g.SCRIPTS), "-postScript", "A.py", "1", "-postScript",
                                     "B.py"])


# A position-independent x86-64 ELF: one PT_LOAD at 0, code at CODE. The entry calls four
# functions, so Ghidra's analysis finds them and gives them its default names. Ghidra rebases
# the image to 0x100000; functions.csv addresses stay relative to that.
CODE = 0x1000
FUNCS = [0x1020, 0x1040, 0x1060, 0x10a0]  # 0x10a0 keeps its default name: not exported
UNCALLED = 0x1080  # analysis does not find it: the import creates it
FUNC_BODY = bytes.fromhex("89f801f0c3")  # mov eax, edi; add eax, esi; ret


def build_elf() -> bytes:
    image = bytearray(0x2000)
    entry = bytearray()
    for target in FUNCS:
        entry += b"\xe8" + struct.pack("<i", target - (CODE + len(entry) + 5))
    image[CODE:CODE + len(entry) + 1] = entry + b"\xc3"
    for addr in [*FUNCS, UNCALLED]:
        image[addr:addr + len(FUNC_BODY)] = FUNC_BODY
    ident = b"\x7fELF\x02\x01\x01" + bytes(9)
    image[:0x40] = struct.pack("<16sHHIQQQIHHHHHH", ident, 3, 62, 1, CODE, 0x40, 0, 0, 0x40,
                               0x38, 1, 0x40, 0, 0)
    image[0x40:0x78] = struct.pack("<IIQQQQQQ", 1, 5, 0, 0, 0, len(image), len(image), 0x1000)
    return bytes(image)


# What a first export wrote: our names, a signature only, and a function Ghidra does not find.
FIXTURE = HEADER + "".join([
    "0x00001020,add_two,\"int add_two(int a, int b)\",decompiled,src/game/math.cpp\n",
    "0x00001040,just_named,,named,\n",
    "0x00001060,,long FUN_00101060(long x),unknown,\n",
    "0x00001080,not_called,,named,\n",
])


@unittest.skipUnless(g.local_env("GHIDRA_INSTALL_DIR"),
                     "GHIDRA_INSTALL_DIR not set (environment or local.env)")
class GhidraRoundTripTest(unittest.TestCase):
    def test_round_trip(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / "test.elf").write_bytes(build_elf())
            symbols = tmp / "symbols"
            symbols.mkdir()
            (symbols / "functions.csv").write_text(FIXTURE)
            project = tmp / "proj" / "sync.gpr"
            # import into a fresh project, then export from it in a second session
            with contextlib.redirect_stdout(io.StringIO()):
                g.sync(project, symbols, ["-import", str(tmp / "test.elf")], True, False)
                (symbols / "functions.csv").write_text(HEADER)
                g.sync(project, symbols, ["-process", "test.elf", "-noanalysis", "-readOnly"],
                       False, True)
            exported = g.read_functions(symbols / "functions.csv")
            fixture = table(FIXTURE[len(HEADER):], tmp)
        # Ghidra knows names and signatures only; status and file come from the CSV
        self.assertEqual({a: (r["name"], r["signature"]) for a, r in exported.items()},
                         {a: (r["name"], r["signature"]) for a, r in fixture.items()})
        merged = g.merge_functions(fixture, g.import_rows(exported))
        self.assertEqual(g.format_functions(merged), FIXTURE)


if __name__ == "__main__":
    unittest.main()
