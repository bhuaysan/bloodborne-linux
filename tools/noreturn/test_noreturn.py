#!/usr/bin/env python3
"""Tests for noreturn. Run: python3 -m unittest discover -s tools/noreturn

The synthetic tests need no game files. The game test runs only when BB_GAME_DIR is set
(environment or local.env). It rebuilds the eboot ELF from its SELF in memory.
"""

from __future__ import annotations

import contextlib
import hashlib
import io
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS / "noreturn"))
sys.path.insert(0, str(TOOLS / "self2elf"))
sys.path.insert(0, str(TOOLS / "common"))
import noreturn as nr  # noqa: E402
import cli  # noqa: E402  (on the path through noreturn)
import ps4elf as p  # noqa: E402
import self2elf  # noqa: E402
from elfbuilder import Builder, build_eh_frame, build_eh_frame_hdr  # noqa: E402
from toolenv import load_game_dir  # noqa: E402

TEXT_SIZE, DATA_VADDR = 0x200, 0x4000
EH_FRAME = 0x100  # vaddr of .eh_frame in the text segment, .eh_frame_hdr right after it
STUB_A, STUB_B = 0x1E0, 0x1F0  # PLT stubs of the two imports of the synthetic ELF
SLOT_A, SLOT_B = DATA_VADDR + 0x18, DATA_VADDR + 0x20  # their JUMP_SLOT relocations
NAME_A, NAME_B = "AAAAAAAAAAA#A#B", "BBBBBBBBBBB#A#B"


def build_elf(eh=None) -> bytes:
    """Text at 0 and data at DATA_VADDR, the imports NAME_A and NAME_B with their JUMP_SLOT
    relocations. eh: (eh_frame vaddr, eh_frame bytes, eh_frame_hdr vaddr, eh_frame_hdr bytes,
    further (vaddr, bytes) pairs...) in the text segment."""
    b = (Builder(entry=0x10).needed("libkernel.prx", 1, "libkernel")
         .load(0, bytes([0xCC]) * TEXT_SIZE)
         .load(DATA_VADDR, bytes(0x80), 0x1234, p.PF_R | p.PF_W)
         .sym(NAME_A).sym(NAME_B).sym("module_start")
         .jump_slot(SLOT_A, 1).jump_slot(SLOT_B, 2).table(p.DT_SCE_PLTGOT, DATA_VADDR))
    if eh:
        b.place(eh[0], eh[1]).eh_frame_hdr(eh[2], eh[3])
        for vaddr, chunk in zip(eh[4::2], eh[5::2]):
            b.place(vaddr, chunk)
    return b.build()


def eh_layout(fdes=2):
    """.eh_frame at EH_FRAME with one FDE per function at 0x10, 0x20, ..., the header after it."""
    frame, table = build_eh_frame(EH_FRAME, fdes)
    hdr_vaddr = EH_FRAME + len(frame)
    return EH_FRAME, frame, hdr_vaddr, build_eh_frame_hdr(hdr_vaddr, EH_FRAME, table)


def call(at: int, target: int) -> bytes:
    return b"\xe8" + struct.pack("<i", target - (at + 5))


def stub(at: int, slot: int) -> bytes:
    return b"\xff\x25" + struct.pack("<i", slot - (at + 6))


def function(start: int, *calls: tuple[int, int], tail: bytes = b"\xc3") -> bytes:
    """16 bytes of code at start: push rbp filler, the calls (offset, target), then tail."""
    code = bytearray(b"\x55" * 16)
    code[16 - len(tail):] = tail
    for offset, target in calls:
        code[offset: offset + 5] = call(start + offset, target)
    return bytes(code)


def build(functions: list[bytes]) -> bytes:
    """Synthetic ELF with one FDE per function at 0x10, 0x20, ... and the two PLT stubs."""
    frame_vaddr, frame, hdr_vaddr, hdr = eh_layout(fdes=len(functions))
    code = b"".join(functions)
    assert hdr_vaddr + len(hdr) <= STUB_A and 0x10 + len(code) <= frame_vaddr
    return build_elf(eh=(frame_vaddr, frame, hdr_vaddr, hdr, 0x10, code,
                         STUB_A, stub(STUB_A, SLOT_A), STUB_B, stub(STUB_B, SLOT_B)))


class AnalyzeTests(unittest.TestCase):
    def analyze(self, functions):
        return {imp.symbol: imp for imp in nr.analyze(build(functions))}

    def test_stubs_found(self):
        imports = self.analyze([function(0x10)])
        self.assertEqual({k: v.stubs for k, v in imports.items()},
                         {NAME_A: [STUB_A], NAME_B: [STUB_B]})

    def test_calls_ending_a_function(self):
        imports = self.analyze([
            function(0x10, (11, STUB_A), tail=b""),  # last instruction
            function(0x20, (10, STUB_A), tail=b"\x90"),  # followed by a nop
            function(0x30, (0, STUB_A)),  # in the middle
            function(0x40, (8, STUB_B), tail=b"\x0f\x1f\x00"),  # one ending call only
        ])
        a, b = imports[NAME_A], imports[NAME_B]
        self.assertEqual((a.calls, a.ending, a.noreturn), (3, 2, True))
        self.assertEqual((b.calls, b.ending, b.noreturn), (1, 1, False))

    def test_call_before_other_bytes_does_not_end(self):
        imports = self.analyze([function(0x10, (10, STUB_A), tail=b"\xc3"),
                                function(0x20, (10, STUB_A), tail=b"\xc3")])
        self.assertEqual((imports[NAME_A].calls, imports[NAME_A].ending), (2, 0))

    def test_padding(self):
        self.assertTrue(nr.is_padding(b""))
        self.assertTrue(nr.is_padding(b"\x90\x66\x90\x0f\x1f\x44\x00\x00\xcc"))
        self.assertFalse(nr.is_padding(b"\x90\xc3"))
        self.assertFalse(nr.is_padding(b"\x0f\x1f"))

    def test_rule(self):
        self.assertTrue(nr.Import("x", [], calls=4, ending=2).noreturn)
        self.assertFalse(nr.Import("x", [], calls=5, ending=2).noreturn)
        self.assertFalse(nr.Import("x", [], calls=1, ending=1).noreturn)

    def test_no_eh_frame_rejected(self):
        with self.assertRaisesRegex(nr.ElfError, "PT_GNU_EH_FRAME"):
            nr.analyze(build_elf())

    def test_header_encodings(self):
        # The shared reader (tools/common/ehframe.py) decides which encodings are accepted;
        # noreturn must find the same FDEs through each.
        frame_vaddr, frame, hdr_vaddr, _ = eh_layout()
        for ptr_enc in (0x1B, 0x3B, 0x04):
            with self.subTest(ptr_enc=hex(ptr_enc)):
                _, table = build_eh_frame(frame_vaddr)
                hdr = build_eh_frame_hdr(hdr_vaddr, frame_vaddr, table, ptr_enc=ptr_enc)
                data = build_elf(eh=(frame_vaddr, frame, hdr_vaddr, hdr))
                img = nr.parse(data)
                loads = [ph for ph in img.phdrs if ph.p_type == nr.PT_LOAD]
                self.assertEqual(nr.fde_ranges(data, img.phdrs, loads),
                                 [(func, func + 0x10) for func, _ in table])

    def test_bad_header_encoding_rejected(self):
        frame_vaddr, frame, hdr_vaddr, hdr = eh_layout()
        hdr = hdr[:1] + b"\x9b" + hdr[2:]  # indirect
        with self.assertRaisesRegex(nr.ElfError, "eh_frame_ptr encoding"):
            nr.analyze(build_elf(eh=(frame_vaddr, frame, hdr_vaddr, hdr)))


class MainTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name)
        self.data = build([function(0x10, (11, STUB_A), tail=b""),
                           function(0x20, (11, STUB_A), tail=b"")])
        (self.tmp / "in").mkdir()
        self.input = self.tmp / "in" / "eboot.elf"
        self.input.write_bytes(self.data)
        game = mock.patch.object(cli, "load_game_dir", return_value=None)
        game.start()
        self.addCleanup(game.stop)

    def run_main(self, *argv: str) -> tuple[int, str]:
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = nr.main(list(argv))
        return code, out.getvalue() + err.getvalue()

    def test_writes_stub_list(self):
        pinned = {hashlib.sha256(self.data).hexdigest(): "eboot.elf"}
        with mock.patch.object(nr, "PINNED_ELFS", pinned):
            code, output = self.run_main(str(self.input), "-o", str(self.tmp / "out"))
        self.assertEqual(code, 0, output)
        self.assertEqual((self.tmp / "out" / "eboot.txt").read_text(), f"{STUB_A:x} {NAME_A}\n")

    def test_unpinned_file_refused(self):
        code, output = self.run_main(str(self.input), "-o", str(self.tmp / "out"))
        self.assertEqual(code, 1)
        self.assertIn("not a pinned file", output)
        self.assertFalse((self.tmp / "out").exists())

    def test_output_next_to_input_refused(self):
        code, output = self.run_main(str(self.input), "-o", str(self.tmp / "in" / "out"))
        self.assertEqual(code, 2)
        self.assertIn("refusing", output)


GAME_DIR = load_game_dir()


@unittest.skipUnless(GAME_DIR, "BB_GAME_DIR not set (environment or local.env)")
class GameTests(unittest.TestCase):
    def test_eboot(self):
        (path,) = [p for p in self2elf.default_inputs(GAME_DIR) if p.name == "eboot.bin"]
        imports = nr.analyze(self2elf.self_to_elf(path.read_bytes()).data)
        found = {imp.symbol.split("#")[0] for imp in imports if imp.noreturn}
        # Of these, our NID matching names Ou3iL1abvng __stack_chk_fail, lKEN2IebgJ0 longjmp,
        # uMei1W9uyNo exit and L1SBTkC+Cvw abort.
        self.assertEqual(found, {"Ou3iL1abvng", "eT2UsmTewbU", "lKEN2IebgJ0", "tQIo+GIPklo",
                                 "ozMAr28BwSY", "MELi-cKqWq0", "uMei1W9uyNo", "L1SBTkC+Cvw"})
        self.assertTrue(all(len(imp.stubs) == 1 for imp in imports))


if __name__ == "__main__":
    unittest.main()
