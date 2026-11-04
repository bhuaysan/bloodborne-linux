#!/usr/bin/env python3
"""Tests for elf2std. Run: python3 -m unittest discover -s tools/elf2std

The synthetic tests need no game files. The game test runs only when BB_GAME_DIR is set
(environment or local.env). It rebuilds the ELFs from the SELFs in memory, so it does not
need analysis/elf/.
"""

from __future__ import annotations

import contextlib
import hashlib
import io
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS / "elf2std"))
sys.path.insert(0, str(TOOLS / "self2elf"))
import elf2std as e  # noqa: E402
import self2elf  # noqa: E402

PHDR_OFF = 0x40
TEXT_OFF, TEXT_VADDR, TEXT_SIZE = 0x1000, 0x0, 0x200
DATA_OFF, DATA_VADDR, DATA_SIZE, DATA_MEMSZ = 0x2000, 0x4000, 0x80, 0x1234
DYNLIB_OFF = 0x3000

NAMES = [b"", b"AAAAAAAAAAA#A#B", b"BBBBBBBBBBB#A#B", b"module_start"]


def build_dynlib(extra_tags=(), drop_tags=(), override=None):
    """A dynlibdata segment with string, symbol, relocation and dynamic tables.

    Returns (blob, dynamic table offset inside the blob)."""
    strtab = bytearray(b"\0libkernel.prx\0")
    needed = 1
    name_offs = []
    for name in NAMES:
        if not name:
            name_offs.append(0)
            continue
        name_offs.append(len(strtab))
        strtab += name + b"\0"
    while len(strtab) % 8:
        strtab += b"\0"
    symtab = b"".join(e.SYM.pack(off, (1 << 4) | 2, 0, 0, 0, 0) for off in name_offs)
    jmprel = (struct.pack("<QQq", DATA_VADDR + 0x18, (1 << 32) | 7, 0)
              + struct.pack("<QQq", DATA_VADDR + 0x20, (2 << 32) | 7, 0))
    rela = struct.pack("<QQq", DATA_VADDR + 0x40, 8, 0x100)
    sony_hash = bytes(0x10)  # contents are ignored by the converter

    blob = bytearray(bytes(0x10))  # leading padding, so offsets are non-zero
    def put(chunk):
        off = len(blob)
        blob.extend(chunk)
        return off
    hash_off = put(sony_hash)
    str_off = put(strtab)
    sym_off = put(symtab)
    jmp_off = put(jmprel)
    rela_off = put(rela)

    tags = {
        e.DT_SCE_HASH: hash_off, e.DT_SCE_HASHSZ: len(sony_hash),
        e.DT_SCE_PLTGOT: DATA_VADDR,
        e.DT_SCE_JMPREL: jmp_off, e.DT_SCE_PLTREL: e.DT_RELA, e.DT_SCE_PLTRELSZ: len(jmprel),
        e.DT_SCE_RELA: rela_off, e.DT_SCE_RELASZ: len(rela), e.DT_SCE_RELAENT: e.RELA_SIZE,
        e.DT_SCE_STRTAB: str_off, e.DT_SCE_STRSZ: len(strtab),
        e.DT_SCE_SYMTAB: sym_off, e.DT_SCE_SYMENT: e.SYM.size, e.DT_SCE_SYMTABSZ: len(symtab),
    }
    tags.update(override or {})
    entries = [(1, needed), (0xD, 0x80)]  # DT_NEEDED, DT_FINI
    entries += [(t, v) for t, v in tags.items() if t not in drop_tags]
    entries += [(0x6100000D, 0x1234), *extra_tags, (0, 0)]  # a Sony module tag
    dyn_off = put(b"".join(e.DYN.pack(t, v) for t, v in entries))
    return bytes(blob), dyn_off


def build_eh_frame(vaddr: int, fdes: int = 2, terminator: bool = True,
                   extended: bool = False) -> tuple[bytes, list[tuple[int, int]]]:
    """.eh_frame at vaddr: one CIE, then FDEs (pointer encoding pcrel|sdata4) for the functions
    at 0x10, 0x20, ... (16 bytes each). Returns the bytes and (function, FDE vaddr) pairs."""
    def record(body: bytes) -> bytes:
        body += bytes(-(len(body) + 4) % 4)  # DW_CFA_nop padding
        if extended:
            return struct.pack("<IQ", 0xFFFFFFFF, len(body)) + body
        return struct.pack("<I", len(body)) + body
    # id 0, version 1, "zR", code align 1, data align -8, return address r16, R = 0x1b
    out = bytearray(record(struct.pack("<I", 0) + b"\x01zR\x00\x01\x78\x10\x01\x1b"))
    table = []
    id_field = 12 if extended else 4  # offset of the CIE id / CIE pointer in a record
    for i in range(fdes):
        func, here = 0x10 * (i + 1), vaddr + len(out)
        table.append((func, here))
        pc_field = here + id_field + 4
        out += record(struct.pack("<Iii", len(out) + id_field, func - pc_field, 0x10) + b"\x00")
    if terminator:
        out += bytes(4)
    return bytes(out), table


def build_eh_frame_hdr(vaddr: int, eh_frame: int, table: list[tuple[int, int]],
                       ptr_enc: int = 0x1B) -> bytes:
    """.eh_frame_hdr at vaddr pointing at eh_frame, with a datarel|sdata4 search table."""
    if ptr_enc == 0x1B:
        ptr = struct.pack("<i", eh_frame - (vaddr + 4))
    elif ptr_enc == 0x3B:
        ptr = struct.pack("<i", eh_frame - vaddr)
    else:
        ptr = struct.pack("<Q", eh_frame)
    return (bytes([1, ptr_enc, 0x03, 0x3B]) + ptr + struct.pack("<I", len(table))
            + b"".join(struct.pack("<ii", func - vaddr, fde - vaddr) for func, fde in table))


def build_elf(e_type=e.ET_SCE_DYNEXEC, eh=None, **kwargs) -> bytes:
    """eh: (eh_frame vaddr, eh_frame bytes, eh_frame_hdr vaddr, eh_frame_hdr bytes) in the text
    segment, described by PT_GNU_EH_FRAME."""
    blob, dyn_off = build_dynlib(**kwargs)
    phdrs = [
        e.Phdr(e.PT_LOAD, 5, TEXT_OFF, TEXT_VADDR, TEXT_VADDR, TEXT_SIZE, TEXT_SIZE, 0x4000),
        e.Phdr(e.PT_LOAD, 6, DATA_OFF, DATA_VADDR, DATA_VADDR, DATA_SIZE, DATA_MEMSZ, 0x4000),
        e.Phdr(e.PT_DYNAMIC, 6, DYNLIB_OFF + dyn_off, 0, 0, len(blob) - dyn_off,
               len(blob) - dyn_off, 8),
        e.Phdr(e.PT_SCE_DYNLIBDATA, 4, DYNLIB_OFF, 0, 0, len(blob), 0, 0x10),
    ]
    if eh:
        hdr_vaddr, hdr = eh[2], eh[3]
        phdrs.append(e.Phdr(e.PT_GNU_EH_FRAME, 4, TEXT_OFF + hdr_vaddr - TEXT_VADDR, hdr_vaddr,
                            hdr_vaddr, len(hdr), len(hdr), 4))
    ident = b"\x7fELF\x02\x01\x01\x09" + bytes(8)
    out = bytearray(DYNLIB_OFF + len(blob))
    out[:0x40] = e.ELF_HEADER.pack(ident, e_type, e.EM_X86_64, 1, 0x10, PHDR_OFF, 0, 0, 0x40,
                                   e.ELF_PHDR.size, len(phdrs), 0x40, 0, 0)
    for i, ph in enumerate(phdrs):
        out[PHDR_OFF + i * e.ELF_PHDR.size: PHDR_OFF + (i + 1) * e.ELF_PHDR.size] = ph.pack()
    out[TEXT_OFF: TEXT_OFF + TEXT_SIZE] = bytes([0xCC]) * TEXT_SIZE
    for vaddr, chunk in zip(eh[::2], eh[1::2]) if eh else ():
        out[TEXT_OFF + vaddr - TEXT_VADDR: TEXT_OFF + vaddr - TEXT_VADDR + len(chunk)] = chunk
    out[DATA_OFF: DATA_OFF + DATA_SIZE] = bytes(range(DATA_SIZE))
    out[DYNLIB_OFF:] = blob
    return bytes(out)


class Image:
    """Minimal reader for the converted output, via program headers only."""

    def __init__(self, data: bytes):
        self.data = data
        header = e.ELF_HEADER.unpack_from(data)
        self.e_type, phoff, phnum = header[1], header[5], header[10]
        self.phdrs = [e.Phdr(*e.ELF_PHDR.unpack_from(data, phoff + i * e.ELF_PHDR.size))
                      for i in range(phnum)]

    def read(self, vaddr: int, size: int) -> bytes:
        for ph in self.phdrs:
            if ph.p_type == e.PT_LOAD and ph.p_vaddr <= vaddr and vaddr + size <= ph.p_vaddr + ph.p_filesz:
                off = ph.p_offset + vaddr - ph.p_vaddr
                return self.data[off: off + size]
        raise AssertionError(f"{vaddr:#x} not mapped")

    def sections(self) -> list[tuple]:
        """(name, type, flags, addr, offset, size, link, info, align, entsize) per section."""
        header = e.ELF_HEADER.unpack_from(self.data)
        shoff, shentsize, shnum, shstrndx = header[6], header[11], header[12], header[13]
        raw = [e.ELF_SHDR.unpack_from(self.data, shoff + i * shentsize) for i in range(shnum)]
        strtab = raw[shstrndx][4] if raw else 0
        def name(off):
            return self.data[strtab + off: self.data.index(b"\0", strtab + off)]
        return [(name(sh[0]), *sh[1:]) for sh in raw]

    def dynamic(self) -> list[tuple[int, int]]:
        (dyn,) = [ph for ph in self.phdrs if ph.p_type == e.PT_DYNAMIC]
        raw = self.read(dyn.p_vaddr, dyn.p_filesz)
        return [e.DYN.unpack_from(raw, i) for i in range(0, len(raw), e.DYN.size)]

    def cstr(self, vaddr: int) -> bytes:
        out = bytearray()
        while (c := self.read(vaddr + len(out), 1)) != b"\0":
            out += c
        return bytes(out)

    def lookup(self, dyn: dict[int, int], name: bytes) -> int:
        """Look up a symbol index through DT_HASH, like a dynamic linker would."""
        nbucket, nchain = struct.unpack("<II", self.read(dyn[e.DT_HASH], 8))
        table = struct.unpack(f"<{nbucket + nchain}I",
                              self.read(dyn[e.DT_HASH] + 8, 4 * (nbucket + nchain)))
        index = table[e.sysv_hash(name) % nbucket]
        while index:
            sym = e.SYM.unpack(self.read(dyn[e.DT_SYMTAB] + index * e.SYM.size, e.SYM.size))
            if self.cstr(dyn[e.DT_STRTAB] + sym[0]) == name:
                return index
            index = table[nbucket + index]
        return 0


class SyntheticTests(unittest.TestCase):
    def setUp(self):
        self.src = build_elf()
        self.result = e.convert(self.src)
        self.img = Image(self.result.data)
        self.dyn = dict(self.img.dynamic())

    def test_header(self):
        self.assertEqual(self.img.e_type, e.ET_EXEC)
        self.assertEqual(len(self.img.phdrs), 5)
        self.assertEqual(self.result.symbols, len(NAMES))
        self.assertEqual((self.result.plt_relocs, self.result.relocs), (2, 1))

    def test_new_load_segment(self):
        new = self.img.phdrs[-1]
        self.assertEqual(new.p_type, e.PT_LOAD)
        # First free page above the highest loaded byte (data memsz, not filesz).
        self.assertEqual(new.p_vaddr, e.align_up(DATA_VADDR + DATA_MEMSZ, e.SEGMENT_ALIGN))
        self.assertEqual(new.p_offset % e.SEGMENT_ALIGN, 0)
        blob, _ = build_dynlib()
        self.assertEqual(self.img.read(new.p_vaddr, len(blob)), blob)

    def test_original_bytes_kept(self):
        start = PHDR_OFF + 5 * e.ELF_PHDR.size
        self.assertEqual(self.result.data[start: len(self.src)], self.src[start:])
        for i, (old, new) in enumerate(zip(build_elf_phdrs(self.src), self.img.phdrs)):
            if old.p_type != e.PT_DYNAMIC:
                self.assertEqual(old, new, f"phdr {i}")

    def test_dynamic_tags(self):
        tags = [t for t, _ in self.img.dynamic()]
        self.assertEqual(tags[-1], e.DT_NULL)
        self.assertFalse([t for t in tags if t >= e.DT_LOOS], "Sony tags left in the table")
        self.assertEqual(self.dyn[e.DT_PLTGOT], DATA_VADDR)
        self.assertEqual(self.dyn[e.DT_PLTREL], e.DT_RELA)
        self.assertEqual(self.dyn[0xD], 0x80)  # DT_FINI kept
        self.assertEqual(self.img.cstr(self.dyn[e.DT_STRTAB] + self.dyn[1]), b"libkernel.prx")

    def test_relocations_readable(self):
        jmprel = self.img.read(self.dyn[e.DT_JMPREL], self.dyn[e.DT_PLTRELSZ])
        offsets = [struct.unpack_from("<Q", jmprel, i)[0] for i in range(0, len(jmprel), 24)]
        self.assertEqual(offsets, [DATA_VADDR + 0x18, DATA_VADDR + 0x20])
        rela = self.img.read(self.dyn[e.DT_RELA], self.dyn[e.DT_RELASZ])
        self.assertEqual(struct.unpack("<QQq", rela), (DATA_VADDR + 0x40, 8, 0x100))

    def test_hash_finds_every_symbol(self):
        for index, name in enumerate(NAMES[1:], start=1):
            self.assertEqual(self.img.lookup(self.dyn, name), index)
        self.assertEqual(self.img.lookup(self.dyn, b"missing"), 0)

    def test_dynamic_segment_read_only(self):
        (dyn,) = [ph for ph in self.img.phdrs if ph.p_type == e.PT_DYNAMIC]
        self.assertEqual(dyn.p_flags, e.PF_R)

    def test_prx_type(self):
        self.assertEqual(Image(e.convert(build_elf(e.ET_SCE_DYNAMIC)).data).e_type, e.ET_EXEC)

    def test_sysv_hash_vectors(self):
        # Values from the System V ABI hash function (cross-checked against libelf's elf_hash).
        self.assertEqual(e.sysv_hash(b""), 0)
        self.assertEqual(e.sysv_hash(b"printf"), 0x077905A6)
        self.assertEqual(e.sysv_hash(b"module_start"), 0x0E2F14B4)


EH_FRAME = 0x100  # vaddr of .eh_frame in the text segment


def eh_layout(fdes=2, terminator=True, extended=False, ptr_enc=0x1B):
    """.eh_frame at EH_FRAME, .eh_frame_hdr right after it."""
    frame, table = build_eh_frame(EH_FRAME, fdes, terminator, extended)
    hdr_vaddr = EH_FRAME + len(frame)
    return EH_FRAME, frame, hdr_vaddr, build_eh_frame_hdr(hdr_vaddr, EH_FRAME, table, ptr_enc)


class EhFrameTests(unittest.TestCase):
    def convert(self, eh):
        result = e.convert(build_elf(eh=eh))
        img = Image(result.data)
        return result, {sec[0]: sec for sec in img.sections()}, img

    def test_sections(self):
        eh = eh_layout()
        result, secs, img = self.convert(eh)
        self.assertEqual(result.eh_frame, (EH_FRAME, len(eh[1])))
        self.assertEqual(result.fdes, 2)
        self.assertEqual(img.sections()[0], (b"", 0, 0, 0, 0, 0, 0, 0, 0, 0))
        self.assertEqual(img.sections()[-1][0], b".shstrtab")
        for name, vaddr, data in ((b".eh_frame", eh[0], eh[1]), (b".eh_frame_hdr", eh[2], eh[3])):
            _, sh_type, flags, addr, offset, size, _, _, align, _ = secs[name]
            self.assertEqual((sh_type, flags, addr, size, align),
                             (e.SHT_PROGBITS, e.SHF_ALLOC, vaddr, len(data), 4))
            self.assertEqual(result.data[offset: offset + size], data)

    def test_every_loaded_byte_has_a_section(self):
        eh = eh_layout()
        _, secs, img = self.convert(eh)
        new = img.phdrs[-1]
        by_addr = sorted((sec for sec in img.sections() if sec[2] & e.SHF_ALLOC),
                         key=lambda sec: sec[3])
        expected = [
            (b".text", e.SHT_PROGBITS, e.SHF_ALLOC | e.SHF_EXECINSTR, TEXT_VADDR, EH_FRAME),
            (b".eh_frame", e.SHT_PROGBITS, e.SHF_ALLOC, EH_FRAME, len(eh[1])),
            (b".eh_frame_hdr", e.SHT_PROGBITS, e.SHF_ALLOC, eh[2], len(eh[3])),
            (b".text", e.SHT_PROGBITS, e.SHF_ALLOC | e.SHF_EXECINSTR, eh[2] + len(eh[3]),
             TEXT_SIZE - (eh[2] + len(eh[3]))),
            (b".data", e.SHT_PROGBITS, e.SHF_ALLOC | e.SHF_WRITE, DATA_VADDR, DATA_SIZE),
            (b".bss", e.SHT_NOBITS, e.SHF_ALLOC | e.SHF_WRITE, DATA_VADDR + DATA_SIZE,
             DATA_MEMSZ - DATA_SIZE),
        ]
        self.assertEqual([sec[:4] + (sec[5],) for sec in by_addr[:6]], expected)
        # The new segment only gets .dynstr and .dynamic (see fill_segments).
        self.assertEqual([sec[0] for sec in by_addr[6:]], [b".dynstr", b".dynamic"])
        dyn = dict(img.dynamic())
        dynstr, dynamic = secs[b".dynstr"], secs[b".dynamic"]
        self.assertEqual((dynstr[1], dynstr[3], dynstr[5]),
                         (e.SHT_STRTAB, dyn[e.DT_STRTAB], dyn[e.DT_STRSZ]))
        (pt_dyn,) = [ph for ph in img.phdrs if ph.p_type == e.PT_DYNAMIC]
        self.assertEqual((dynamic[1], dynamic[3], dynamic[5], dynamic[9]),
                         (e.SHT_DYNAMIC, pt_dyn.p_vaddr, pt_dyn.p_filesz, e.DYN.size))
        self.assertEqual(img.sections()[dynamic[6]][0], b".dynstr")
        self.assertTrue(new.p_vaddr <= dynstr[3] and dynamic[3] + dynamic[5] <= new.p_vaddr
                        + new.p_filesz)

    def test_missing_terminator_before_hdr(self):
        eh = eh_layout(terminator=False)
        result, _, _ = self.convert(eh)
        self.assertEqual(result.eh_frame, (EH_FRAME, len(eh[1])))
        self.assertEqual(result.fdes, 2)

    def test_missing_terminator_at_segment_end(self):
        hdr_vaddr = 0x80
        start = TEXT_VADDR + TEXT_SIZE - len(build_eh_frame(0, 3, terminator=False)[0])
        frame, table = build_eh_frame(start, 3, terminator=False)
        eh = (start, frame, hdr_vaddr, build_eh_frame_hdr(hdr_vaddr, start, table))
        result, secs, _ = self.convert(eh)
        self.assertEqual(result.eh_frame, (start, TEXT_SIZE - start))
        self.assertEqual(result.fdes, 3)
        self.assertLess(secs[b".eh_frame_hdr"][3], secs[b".eh_frame"][3])

    def test_extended_length_and_other_encodings(self):
        for ptr_enc in (0x1B, 0x3B, 0x04):
            with self.subTest(ptr_enc=hex(ptr_enc)):
                eh = eh_layout(extended=True, ptr_enc=ptr_enc)
                result, _, _ = self.convert(eh)
                self.assertEqual((result.eh_frame, result.fdes), ((EH_FRAME, len(eh[1])), 2))

    def test_terminator_only(self):
        result, _, _ = self.convert(eh_layout(fdes=0))
        self.assertEqual(result.fdes, 0)

    def test_no_eh_frame_no_sections(self):
        result = e.convert(build_elf())
        self.assertIsNone(result.eh_frame)
        self.assertEqual(Image(result.data).sections(), [])

    def test_readelf_accepts_output(self):
        readelf = shutil.which("readelf")
        if not readelf:
            self.skipTest("readelf not installed")
        with tempfile.NamedTemporaryFile(suffix=".elf") as tmp:
            tmp.write(e.convert(build_elf(eh=eh_layout())).data)
            tmp.flush()
            run = subprocess.run([readelf, "-S", "-l", "-d", "--debug-dump=frames", "-W",
                                  tmp.name], capture_output=True, text=True)
        self.assertEqual((run.returncode, run.stderr), (0, ""))
        self.assertEqual(run.stdout.count("FDE cie="), 2)

    def assertRejects(self, eh, message):
        with self.assertRaisesRegex(e.ElfError, message):
            e.convert(build_elf(eh=eh))

    def test_bad_version(self):
        eh = eh_layout()
        self.assertRejects((*eh[:3], b"\x02" + eh[3][1:]), "unsupported .eh_frame_hdr version")

    def test_indirect_encoding(self):
        eh = eh_layout()
        self.assertRejects((*eh[:3], b"\x01\x9b" + eh[3][2:]), "unsupported eh_frame_ptr")

    def test_record_crosses_end(self):
        eh = eh_layout(terminator=False)
        frame = bytearray(eh[1])
        frame[0] += 4  # the CIE now overlaps the next record, and the last one the header
        self.assertRejects((eh[0], bytes(frame), *eh[2:]), "bad length")

    def test_pointer_outside_segments(self):
        eh = eh_layout()
        hdr = eh[3][:4] + struct.pack("<i", 0x100000) + eh[3][8:]
        self.assertRejects((*eh[:3], hdr), "not in the file data")


def build_elf_phdrs(data: bytes) -> list[e.Phdr]:
    return Image(data).phdrs


class RejectTests(unittest.TestCase):
    def assertRejects(self, data: bytes, message: str):
        with self.assertRaisesRegex(e.ElfError, message):
            e.convert(data)

    def test_plain_elf(self):
        self.assertRejects(build_elf(e_type=e.ET_EXEC), "unexpected e_type")

    def test_not_elf(self):
        self.assertRejects(b"\0" * 0x100, "not an ELF")

    def test_missing_tag(self):
        self.assertRejects(build_elf(drop_tags={e.DT_SCE_SYMTAB}), "missing dynamic tag")

    def test_table_out_of_range(self):
        self.assertRejects(build_elf(override={e.DT_SCE_STRSZ: 0x10000}), "past end of dynlibdata")

    def test_rel_instead_of_rela(self):
        self.assertRejects(build_elf(override={e.DT_SCE_PLTREL: 17}), "not RELA")

    def test_standard_tag_clash(self):
        self.assertRejects(build_elf(extra_tags=[(e.DT_STRTAB, 0)]), "unexpected standard tag")

    def test_other_os_tag_refused(self):
        self.assertRejects(build_elf(extra_tags=[(0x6FFFFFFB, 1)]), "unsupported dynamic tag")

    def test_no_room_for_phdr(self):
        data = bytearray(build_elf())
        data[PHDR_OFF + 4 * e.ELF_PHDR.size + 8] = 1
        self.assertRejects(bytes(data), "no free space")


class MainTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)
        self.src = build_elf()
        # Pin the synthetic ELF and point the default input directory into the temp dir.
        self.elf_dir = self.tmp / "elf"
        self.elf_dir.mkdir()
        known = {hashlib.sha256(self.src).hexdigest(): "synthetic.elf"}
        for patch in (mock.patch.object(e, "KNOWN_FILES", known),
                      mock.patch.object(e, "DEFAULT_IN", self.elf_dir)):
            patch.start()
            self.addCleanup(patch.stop)

    def run_main(self, *argv: str) -> tuple[int, str]:
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr), contextlib.redirect_stdout(io.StringIO()):
            status = e.main(list(argv))
        return status, stderr.getvalue()

    def test_unknown_file_refused(self):
        path = self.tmp / "in" / "x.elf"
        path.parent.mkdir()
        path.write_bytes(build_elf(e_type=e.ET_SCE_DYNAMIC))
        self.assertEqual(self.run_main(str(path), "-o", str(self.tmp / "out"))[0], 1)
        self.assertFalse((self.tmp / "out").exists())

    def test_unreadable_input_does_not_stop_the_batch(self):
        inputs = self.tmp / "in"
        (inputs / "a-directory").mkdir(parents=True)
        good = inputs / "synthetic.elf"
        good.write_bytes(self.src)
        out = self.tmp / "out"
        status, err = self.run_main(str(inputs / "missing.elf"), str(inputs / "a-directory"),
                                    str(good), "-o", str(out))
        self.assertEqual(status, 1)
        self.assertEqual(err.count("error:"), 2)
        self.assertTrue((out / "synthetic.elf").is_file())

    def test_output_into_self2elf_dir_refused(self):
        other = self.tmp / "copy" / "synthetic.elf"
        other.parent.mkdir()
        other.write_bytes(self.src)
        self.assertEqual(self.run_main(str(other), "-o", str(self.elf_dir))[0], 2)
        self.assertEqual(list(self.elf_dir.iterdir()), [])

    def test_output_next_to_input_refused(self):
        path = self.tmp / "in" / "synthetic.elf"
        path.parent.mkdir()
        path.write_bytes(self.src)
        self.assertEqual(self.run_main(str(path), "-o", str(path.parent / "sub"))[0], 2)

    def test_default_run_ignores_stray_files(self):
        (self.elf_dir / "synthetic.elf").write_bytes(self.src)
        (self.elf_dir / "patched-copy.elf").write_bytes(b"not pinned")
        out = self.tmp / "out"
        self.assertEqual(self.run_main("-o", str(out)), (0, ""))
        self.assertEqual([p.name for p in out.iterdir()], ["synthetic.elf"])

    def test_default_run_reports_missing_pinned_file(self):
        status, err = self.run_main("-o", str(self.tmp / "out"))
        self.assertEqual(status, 1)
        self.assertIn("synthetic.elf", err)


GAME_DIR = e.load_game_dir()


@unittest.skipUnless(GAME_DIR, "BB_GAME_DIR not set (environment or local.env)")
class GameTests(unittest.TestCase):
    def test_all_pinned_elfs(self):
        readelf = shutil.which("readelf")
        converted = set()
        for path in self2elf.default_inputs(GAME_DIR):
            with self.subTest(path.name):
                data = self2elf.self_to_elf(path.read_bytes()).data
                known = e.KNOWN_FILES.get(hashlib.sha256(data).hexdigest())
                self.assertIsNotNone(known, "rebuilt ELF is not pinned in elf2std")
                converted.add(known)
                result = e.convert(data)
                img = Image(result.data)
                dyn = dict(img.dynamic())
                strtab = dyn[e.DT_STRTAB]
                for index in range(1, result.symbols):
                    sym = e.SYM.unpack(img.read(dyn[e.DT_SYMTAB] + index * e.SYM.size, e.SYM.size))
                    name = img.cstr(strtab + sym[0])
                    # Names are not unique (e.g. a weak and a global entry), so only check
                    # that the lookup returns a symbol with this name.
                    self.assertNotEqual(img.lookup(dyn, name), 0, name)
                # In every pinned file .eh_frame ends with a terminator right before
                # .eh_frame_hdr.
                (eh_hdr,) = [ph for ph in img.phdrs if ph.p_type == e.PT_GNU_EH_FRAME]
                self.assertEqual(sum(result.eh_frame), eh_hdr.p_vaddr)
                self.assertEqual(img.read(eh_hdr.p_vaddr - 4, 4), bytes(4))
                self.assertGreater(result.fdes, 0)
                if readelf:
                    with tempfile.NamedTemporaryFile(suffix=".elf") as tmp:
                        tmp.write(result.data)
                        tmp.flush()
                        run = subprocess.run([readelf, "-h", "-l", "-S", "-d", "-r", "--dyn-syms",
                                              "-W", tmp.name], capture_output=True, text=True)
                    self.assertEqual(run.returncode, 0)
                    self.assertEqual(run.stderr, "")
        self.assertEqual(converted, set(e.KNOWN_FILES.values()))


if __name__ == "__main__":
    unittest.main()
