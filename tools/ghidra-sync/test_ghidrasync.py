#!/usr/bin/env python3
"""Tests for ghidrasync. Run: python3 -m unittest discover -s tools/ghidra-sync

The CSV tests need neither Ghidra nor the game. The round trip runs headless Ghidra on a small
ELF built here and skips when GHIDRA_INSTALL_DIR is unset (environment or local.env) or
PyGhidra is not set up.
"""

from __future__ import annotations

import contextlib
import io
import shutil
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
        cmd = g.headless_command("py", Path("/g"), Path("/p/proj.gpr"), ["-process", "x"],
                                 [["A.py", "1"], ["B.py"]])
        self.assertEqual(cmd[:6], ["py", "-m", "pyghidra.ghidra_launch", "--install-dir", "/g",
                                   "ghidra.app.util.headless.AnalyzeHeadless"])
        self.assertEqual(cmd[6:], ["/p", "proj", "-process", "x", "-scriptPath",
                                     str(g.SCRIPTS), "-postScript", "A.py", "1", "-postScript",
                                     "B.py"])


# What an export writes for two subsystems: a struct with gaps, trailing padding and a field off
# its natural alignment, a union, enums of 1 and 8 bytes, typedefs, pointers, arrays, and
# references within a header and across headers, a struct that points to itself via a typedef.
CORE_H = """\
/* Ghidra category /bb/core: written by tools/ghidra-sync, see symbols/README.md */

#pragma pack(push, 1)

struct Vec3;

enum Flags {
    FLAG_NONE = 0,
    FLAG_BIG = 4294967296,
};
_Static_assert(sizeof(enum Flags) == 0x8, "Flags");

enum Kind {
    KIND_NEG = -1,
    KIND_A = 0,
    KIND_B = 2,
};
_Static_assert(sizeof(enum Kind) == 0x1, "Kind");

struct Vec3 {
    /* 0x0000 */ float x;
    /* 0x0004 */ float y;
    /* 0x0008 */ float z;
};
_Static_assert(sizeof(struct Vec3) == 0xc, "Vec3");

typedef uint handle_t;

#pragma pack(pop)
"""

GAME_H = """\
/* Ghidra category /bb/game: written by tools/ghidra-sync, see symbols/README.md */
#include "core.h"

#pragma pack(push, 1)

struct Actor;
union Slot;

typedef struct Actor Actor_t;

union Slot {
    /* 0x0000 */ int i;
    /* 0x0000 */ float f;
    /* 0x0000 */ void *p;
};
_Static_assert(sizeof(union Slot) == 0x8, "Slot");

struct Actor {
    /* 0x0000 */ struct Vec3 pos;
    /* 0x000c */ enum Kind kind;
    /* 0x000d */ undefined1 _pad_0x000d[3];
    /* 0x0010 */ Actor_t *next;
    /* 0x0018 */ handle_t handle;
    /* 0x001c */ undefined1 _pad_0x001c[2];
    /* 0x001e */ uint misaligned;
    /* 0x0022 */ char name[4][2];
    /* 0x002a */ union Slot slots[2];
    /* 0x003a */ enum Flags flags;
    /* 0x0042 */ undefined1 _pad_0x0042[6];
};
_Static_assert(sizeof(struct Actor) == 0x48, "Actor");

#pragma pack(pop)
"""


def ours(subsystem: str, name: str, tag: str = "") -> dict:
    return {"name": name, "tag": tag, "subsystem": subsystem}


def field(offset: int, size: int, name: str, ref: dict) -> dict:
    return {"offset": offset, "size": size, "name": name, "type": ref}


# GAME_H as TypesExport.py describes it, in the order Ghidra lists the types
GAME_JSON = [
    {"kind": "struct", "name": "Actor", "size": 0x48, "fields": [
        field(0x00, 12, "pos", ours("core", "Vec3", "struct")),
        field(0x0c, 1, "kind", ours("core", "Kind", "enum")),
        field(0x10, 8, "next", {"pointer": ours("game", "Actor_t")}),
        field(0x18, 4, "handle", ours("core", "handle_t")),
        field(0x1e, 4, "misaligned", {"name": "uint"}),
        field(0x22, 8, "name", {"array": {"array": {"name": "char"}, "count": 2}, "count": 4}),
        field(0x2a, 16, "slots", {"array": ours("game", "Slot", "union"), "count": 2}),
        field(0x3a, 8, "flags", ours("core", "Flags", "enum")),
    ]},
    {"kind": "typedef", "name": "Actor_t", "type": ours("game", "Actor", "struct")},
    {"kind": "union", "name": "Slot", "size": 8, "fields": [
        field(0, 4, "i", {"name": "int"}), field(0, 4, "f", {"name": "float"}),
        field(0, 8, "p", {"pointer": None})]},
]


class TypesHeaderTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(self.enterContext(tempfile.TemporaryDirectory()))

    def test_format_header(self):
        self.assertEqual(g.format_header("game", GAME_JSON), GAME_H)
        self.assertEqual(g.format_header("game", GAME_JSON[::-1]), GAME_H)
        user = {"kind": "struct", "name": "A", "size": 1,
                "fields": [field(0, 1, "a", ours("game", "Actor_t"))]}
        text = g.format_header("game", [user, *GAME_JSON])
        self.assertLess(text.index("struct Actor {"), text.index("struct A {"))

    def test_enum_values_sorted_by_value(self):
        enum = {"kind": "enum", "name": "Kind", "size": 1,
                "values": [["KIND_C", 2], ["KIND_B", 2], ["KIND_A", 0]]}
        self.assertIn("    KIND_A = 0,\n    KIND_B = 2,\n    KIND_C = 2,\n",
                      g.format_header("core", [enum]))

    def test_overlap_and_cycles_fail(self):
        overlap = {"kind": "struct", "name": "S", "size": 8, "fields": [
            field(0, 4, "a", {"name": "int"}), field(2, 4, "b", {"name": "int"})]}
        loop = [{"kind": "struct", "name": n, "size": 4,
                 "fields": [field(0, 4, "x", ours("core", m, "struct"))]}
                for n, m in (("A", "B"), ("B", "A"))]
        union = {"kind": "union", "name": "U", "size": 8,  # no member covers all 8 bytes
                 "fields": [field(0, 5, "c", {"array": {"name": "char"}, "count": 5})]}
        for items in ([overlap], loop, [union]):
            with self.subTest(items=items), self.assertRaises(g.SyncError):
                g.format_header("core", items)

    def test_read_headers(self):
        types = self.tmp / "types"
        g.write_types(types, {"game": GAME_JSON})
        (types / "core.h").write_text(CORE_H)
        headers = g.read_headers(types)
        self.assertEqual([h["subsystem"] for h in headers], ["core", "game"])
        self.assertEqual(headers[0]["sizes"], {"Flags": 8, "Kind": 1, "Vec3": 12})
        self.assertEqual(headers[1]["sizes"], {"Slot": 8, "Actor": 0x48})
        self.assertEqual(headers[0]["enums"], {
            "Flags": [["FLAG_NONE", 0], ["FLAG_BIG", 1 << 32]],
            "Kind": [["KIND_NEG", -1], ["KIND_A", 0], ["KIND_B", 2]]})
        self.assertNotIn("#include", headers[1]["text"])
        self.assertEqual(headers[1]["text"].count("\n"), GAME_H.count("\n"))
        (types / "core.h").write_text('#include "game.h"\n')
        with self.assertRaises(g.SyncError):
            g.read_headers(types)

    def test_write_types_fails_before_writing(self):
        def ptr(to: str) -> list[dict]:
            return [{"kind": "typedef", "name": "P", "type": {"pointer": ours(to, "S", "struct")}}]
        same = {"kind": "struct", "name": "S", "size": 4, "fields": []}
        for exported in ({"a/b": []}, {"a": ptr("b"), "b": ptr("a")},
                         {"a": [same], "b": [same]}):
            with self.subTest(exported=exported), self.assertRaises(g.SyncError):
                g.write_types(self.tmp, exported)
        self.assertEqual(list(self.tmp.iterdir()), [])


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


def pyghidra_missing() -> str:
    """Why the round trip cannot run here, or an empty string."""
    try:
        g.pyghidra_python(g.ghidra_dir())
    except g.SyncError as err:
        return str(err)
    return ""


@unittest.skipIf(MISSING := pyghidra_missing(), MISSING)
class GhidraRoundTripTest(unittest.TestCase):
    def test_round_trip(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / "test.elf").write_bytes(build_elf())
            symbols = tmp / "symbols"
            symbols.mkdir()
            (symbols / "functions.csv").write_text(FIXTURE)
            (symbols / "types").mkdir()
            (symbols / "types" / "core.h").write_text(CORE_H)
            (symbols / "types" / "game.h").write_text(GAME_H)
            project = tmp / "proj" / "sync.gpr"
            # import into a fresh project, then export from it in a second session
            with contextlib.redirect_stdout(io.StringIO()):
                g.sync(project, symbols, ["-import", str(tmp / "test.elf")], True, False)
                (symbols / "functions.csv").write_text(HEADER)
                shutil.rmtree(symbols / "types")
                g.sync(project, symbols, ["-process", "test.elf", "-noanalysis", "-readOnly"],
                       False, True)
            # importing again replaces our types in place and changes nothing
            with contextlib.redirect_stdout(io.StringIO()):
                g.sync(project, symbols, ["-process", "test.elf", "-noanalysis"], True, True)
            exported = g.read_functions(symbols / "functions.csv")
            fixture = table(FIXTURE[len(HEADER):], tmp)
            headers = {p.name: p.read_text() for p in (symbols / "types").iterdir()}
        self.assertEqual(headers, {"core.h": CORE_H, "game.h": GAME_H})
        # Ghidra knows names and signatures only; status and file come from the CSV
        self.assertEqual({a: (r["name"], r["signature"]) for a, r in exported.items()},
                         {a: (r["name"], r["signature"]) for a, r in fixture.items()})
        merged = g.merge_functions(fixture, g.import_rows(exported))
        self.assertEqual(g.format_functions(merged), FIXTURE)


if __name__ == "__main__":
    unittest.main()
