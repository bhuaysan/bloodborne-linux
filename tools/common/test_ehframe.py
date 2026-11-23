"""Tests for the .eh_frame_hdr reader in ehframe.py: every accepted encoding and the rejections."""

from __future__ import annotations

import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ehframe as eh  # noqa: E402
from ps4elf import PT_GNU_EH_FRAME, PT_LOAD, ElfError, Phdr  # noqa: E402

LOAD_VADDR = 0x10000
LOAD_OFF = 0x40  # file offset of the PT_LOAD
HDR_AT = 0x100  # .eh_frame_hdr, relative to the PT_LOAD
EH_FRAME = LOAD_VADDR + 0x20
HDR = LOAD_VADDR + HDR_AT
TABLE = [(LOAD_VADDR + 0x400, LOAD_VADDR + 0x30), (LOAD_VADDR + 0x500, LOAD_VADDR + 0x48)]
# Targets after the header, which unsigned relative encodings can reach.
EH_FRAME_AFTER = LOAD_VADDR + 0x180
TABLE_AFTER = [(LOAD_VADDR + 0x400, LOAD_VADDR + 0x190), (LOAD_VADDR + 0x500, LOAD_VADDR + 0x1A8)]


def encode(value: int, field_vaddr: int, enc: int) -> bytes:
    """value encoded as enc at field_vaddr (datarel base HDR)."""
    base = {eh.EH_PE_ABSPTR: 0, eh.EH_PE_PCREL: field_vaddr, eh.EH_PE_DATAREL: HDR}[enc & 0xF0]
    fmt = eh.EH_PE_FORMATS[enc & 0x0F]
    delta = value - base
    if fmt.isupper():  # unsigned: a negative offset wraps, as the reader's 64-bit sum does
        delta %= 1 << (8 * struct.calcsize(fmt))
    return struct.pack(fmt, delta)


def build_hdr(ptr_enc=0x1B, count_enc=0x03, table_enc=0x3B, table=TABLE, version=1,
              eh_frame=EH_FRAME) -> bytes:
    out = bytearray([version, ptr_enc, count_enc, table_enc])
    out += encode(eh_frame, HDR + len(out), ptr_enc)
    if count_enc != eh.EH_PE_OMIT:
        out += struct.pack(eh.EH_PE_FORMATS[count_enc], len(table))
    if table_enc != eh.EH_PE_OMIT:
        for pair in table:
            for value in pair:
                out += encode(value, HDR + len(out), table_enc)
    return bytes(out)


def image(hdr: bytes, hdr_filesz: int | None = None, eh_phdrs: int = 1):
    """(data, phdrs, loads) with hdr at HDR inside one PT_LOAD of 0x200 bytes."""
    size = 0x200
    data = bytearray(LOAD_OFF + size)
    data[LOAD_OFF + HDR_AT: LOAD_OFF + HDR_AT + len(hdr)] = hdr
    load = Phdr(PT_LOAD, 5, LOAD_OFF, LOAD_VADDR, LOAD_VADDR, size, size, 0x4000)
    filesz = len(hdr) if hdr_filesz is None else hdr_filesz
    eh_phdr = Phdr(PT_GNU_EH_FRAME, 4, LOAD_OFF + HDR_AT, HDR, HDR, filesz, filesz, 4)
    return bytes(data), [load] + [eh_phdr] * eh_phdrs, [load]


def read(hdr: bytes, **kwargs):
    return eh.read_eh_frame_hdr(*image(hdr, **kwargs))


class ReadTests(unittest.TestCase):
    def test_pinned_encodings(self):
        result = read(build_hdr())
        self.assertEqual((result.eh_frame, result.table), (EH_FRAME, TABLE))
        self.assertEqual((result.phdr.p_vaddr, result.load.p_vaddr), (HDR, LOAD_VADDR))

    def test_every_pointer_encoding(self):
        for fmt in (0x00, 0x03, 0x04, 0x0B, 0x0C):
            for app in (eh.EH_PE_ABSPTR, eh.EH_PE_PCREL, eh.EH_PE_DATAREL):
                with self.subTest(enc=hex(app | fmt)):
                    result = read(build_hdr(ptr_enc=app | fmt, table_enc=app | fmt,
                                            table=TABLE_AFTER, eh_frame=EH_FRAME_AFTER))
                    self.assertEqual((result.eh_frame, result.table),
                                     (EH_FRAME_AFTER, TABLE_AFTER))

    def test_count_formats(self):
        for count_enc in (0x00, 0x03, 0x04, 0x0B, 0x0C):
            with self.subTest(count_enc=hex(count_enc)):
                self.assertEqual(read(build_hdr(count_enc=count_enc)).table, TABLE)

    def test_negative_pcrel_wraps_like_the_cpu(self):
        # sdata4 pcrel backwards from the field, as in every pinned file
        hdr = build_hdr(ptr_enc=0x1B)
        self.assertLess(struct.unpack_from("<i", hdr, 4)[0], 0)
        self.assertEqual(read(hdr).eh_frame, EH_FRAME)

    def test_unsigned_relative_does_not_wrap(self):
        # udata4 is zero-extended before the base is added, so it can't point backwards
        hdr = bytearray(build_hdr(ptr_enc=0x13))
        struct.pack_into("<I", hdr, 4, 0xFFFFFFF0)
        self.assertEqual(read(bytes(hdr)).eh_frame, HDR + 4 + 0xFFFFFFF0)

    def test_omitted_table(self):
        for count_enc, table_enc in ((eh.EH_PE_OMIT, 0x3B), (0x03, eh.EH_PE_OMIT),
                                     (eh.EH_PE_OMIT, eh.EH_PE_OMIT)):
            with self.subTest(count_enc=hex(count_enc), table_enc=hex(table_enc)):
                result = read(build_hdr(count_enc=count_enc, table_enc=table_enc))
                self.assertEqual((result.eh_frame, result.table), (EH_FRAME, []))

    def test_empty_table(self):
        self.assertEqual(read(build_hdr(table=[])).table, [])

    def test_no_header(self):
        data, phdrs, loads = image(build_hdr())
        self.assertIsNone(eh.read_eh_frame_hdr(data, phdrs[:1], loads))

    def assertRejects(self, message, hdr, **kwargs):
        with self.assertRaisesRegex(ElfError, message):
            read(hdr, **kwargs)

    def test_rejected_pointer_encodings(self):
        # uleb128, udata2, sleb128, sdata2; indirect, textrel, funcrel, aligned
        for enc in (0x01, 0x02, 0x09, 0x0A, 0x9B, 0x2B, 0x4B, 0x50):
            with self.subTest(enc=hex(enc)):
                hdr = build_hdr()
                self.assertRejects("unsupported eh_frame_ptr encoding",
                                   hdr[:1] + bytes([enc]) + hdr[2:])
                self.assertRejects("unsupported search table encoding",
                                   hdr[:3] + bytes([enc]) + hdr[4:])

    def test_rejected_count_encodings(self):
        for enc in (0x01, 0x13, 0x33):  # uleb128, pcrel, datarel
            with self.subTest(enc=hex(enc)):
                hdr = build_hdr()
                self.assertRejects("unsupported fde_count encoding",
                                   hdr[:2] + bytes([enc]) + hdr[3:])

    def test_negative_count(self):
        hdr = bytearray(build_hdr(count_enc=0x0B))
        struct.pack_into("<i", hdr, 8, -1)
        self.assertRejects("negative fde_count", bytes(hdr))

    def test_bad_version(self):
        self.assertRejects("version 2", build_hdr(version=2))

    def test_two_headers(self):
        self.assertRejects("more than one", build_hdr(), eh_phdrs=2)

    def test_truncated(self):
        hdr = build_hdr()
        for filesz in (3, 7, 11, len(hdr) - 1):
            with self.subTest(filesz=filesz):
                self.assertRejects("too small", hdr, hdr_filesz=filesz)

    def test_outside_load(self):
        self.assertRejects("not in the file data", build_hdr(), hdr_filesz=0x200)


class PointerTests(unittest.TestCase):
    def test_fde_pointers_have_no_data_base(self):
        self.assertEqual(eh.pointer_format(0x1B, "FDE"), "<i")
        with self.assertRaisesRegex(ElfError, "unsupported FDE encoding 0x3b"):
            eh.pointer_format(0x3B, "FDE")

    def test_read_pointer(self):
        data = struct.pack("<i", -0x10)
        self.assertEqual(eh.read_pointer(data, 0, 0x1000, 0x1B), 0xFF0)
        self.assertEqual(eh.read_pointer(data, 0, 0x1000, 0x0B), 2**64 - 0x10)
        self.assertEqual(eh.read_pointer(data, 0, 0x1000, 0x3B, 0x2000), 0x1FF0)


if __name__ == "__main__":
    unittest.main()
