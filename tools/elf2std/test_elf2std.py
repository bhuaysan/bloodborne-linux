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
sys.path.insert(0, str(TOOLS / "common"))
import elf2std as e  # noqa: E402
import ps4elf as p  # noqa: E402
import self2elf  # noqa: E402
from elfbuilder import R_X86_64_RELATIVE, Builder, build_eh_frame, build_eh_frame_hdr  # noqa: E402
from toolenv import load_game_dir  # noqa: E402

PHDR_OFF = 0x40
TEXT_VADDR, TEXT_SIZE = 0x0, 0x200
DATA_VADDR, DATA_SIZE, DATA_MEMSZ = 0x4000, 0x80, 0x1234

NAMES = [b"", b"AAAAAAAAAAA#A#B", b"BBBBBBBBBBB#A#B", b"module_start"]


def builder(e_type=p.ET_SCE_DYNEXEC, eh=None, extra_tags=(), drop_tags=(), override=None):
    """Text and data segments, two PLT and one other relocation, the Sony hash table.

    eh: (eh_frame vaddr, eh_frame bytes, eh_frame_hdr vaddr, eh_frame_hdr bytes, further
    (vaddr, bytes) pairs...) in the text segment; the header gets PT_GNU_EH_FRAME."""
    b = (Builder(e_type=e_type, entry=0x10)
         .needed("libkernel.prx", 1, "libkernel").tag(0xD, 0x80)  # DT_FINI
         .load(TEXT_VADDR, bytes([0xCC]) * TEXT_SIZE)
         .load(DATA_VADDR, bytes(range(DATA_SIZE)), DATA_MEMSZ, p.PF_R | p.PF_W)
         .jump_slot(DATA_VADDR + 0x18, 1).jump_slot(DATA_VADDR + 0x20, 2)
         .rela(DATA_VADDR + 0x40, R_X86_64_RELATIVE, addend=0x100)
         .sony_hash().table(p.DT_SCE_PLTGOT, DATA_VADDR))
    for name in NAMES[1:]:
        b.sym(name.decode())
    for tag, value in extra_tags:
        b.tag(tag, value)
    for tag in drop_tags:
        b.table(tag, None)
    for tag, value in (override or {}).items():
        b.table(tag, value)
    if eh:
        b.place(eh[0], eh[1]).eh_frame_hdr(eh[2], eh[3])
        for vaddr, chunk in zip(eh[4::2], eh[5::2]):
            b.place(vaddr, chunk)
    return b


def build_elf(**kwargs) -> bytes:
    return builder(**kwargs).build()


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
            if ph.p_type == p.PT_LOAD and ph.p_vaddr <= vaddr and vaddr + size <= ph.p_vaddr + ph.p_filesz:
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
        (dyn,) = [ph for ph in self.phdrs if ph.p_type == p.PT_DYNAMIC]
        raw = self.read(dyn.p_vaddr, dyn.p_filesz)
        return [p.DYN.unpack_from(raw, i) for i in range(0, len(raw), p.DYN.size)]

    def cstr(self, vaddr: int) -> bytes:
        out = bytearray()
        while (c := self.read(vaddr + len(out), 1)) != b"\0":
            out += c
        return bytes(out)

    def lookup(self, dyn: dict[int, int], name: bytes) -> int:
        """Look up a symbol index through DT_HASH, like a dynamic linker would."""
        nbucket, nchain = struct.unpack("<II", self.read(dyn[p.DT_HASH], 8))
        table = struct.unpack(f"<{nbucket + nchain}I",
                              self.read(dyn[p.DT_HASH] + 8, 4 * (nbucket + nchain)))
        index = table[e.sysv_hash(name) % nbucket]
        while index:
            sym = p.SYM.unpack(self.read(dyn[p.DT_SYMTAB] + index * p.SYM.size, p.SYM.size))
            if self.cstr(dyn[p.DT_STRTAB] + sym[0]) == name:
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
        self.assertEqual(self.img.e_type, p.ET_EXEC)
        self.assertEqual(len(self.img.phdrs), 5)
        self.assertEqual(self.result.symbols, len(NAMES))
        self.assertEqual((self.result.plt_relocs, self.result.relocs), (2, 1))

    def test_new_load_segment(self):
        new = self.img.phdrs[-1]
        self.assertEqual(new.p_type, p.PT_LOAD)
        # First free page above the highest loaded byte (data memsz, not filesz).
        self.assertEqual(new.p_vaddr, e.align_up(DATA_VADDR + DATA_MEMSZ, e.SEGMENT_ALIGN))
        self.assertEqual(new.p_offset % e.SEGMENT_ALIGN, 0)
        blob, _ = builder().dynlib()
        self.assertEqual(self.img.read(new.p_vaddr, len(blob)), blob)

    def test_original_bytes_kept(self):
        start = PHDR_OFF + 5 * e.ELF_PHDR.size
        self.assertEqual(self.result.data[start: len(self.src)], self.src[start:])
        for i, (old, new) in enumerate(zip(build_elf_phdrs(self.src), self.img.phdrs)):
            if old.p_type != p.PT_DYNAMIC:
                self.assertEqual(old, new, f"phdr {i}")

    def test_dynamic_tags(self):
        tags = [t for t, _ in self.img.dynamic()]
        self.assertEqual(tags[-1], p.DT_NULL)
        self.assertFalse([t for t in tags if t >= p.DT_LOOS], "Sony tags left in the table")
        self.assertEqual(self.dyn[p.DT_PLTGOT], DATA_VADDR)
        self.assertEqual(self.dyn[p.DT_PLTREL], p.DT_RELA)
        self.assertEqual(self.dyn[0xD], 0x80)  # DT_FINI kept
        self.assertEqual(self.img.cstr(self.dyn[p.DT_STRTAB] + self.dyn[1]), b"libkernel.prx")

    def test_relocations_readable(self):
        jmprel = self.img.read(self.dyn[p.DT_JMPREL], self.dyn[p.DT_PLTRELSZ])
        offsets = [struct.unpack_from("<Q", jmprel, i)[0] for i in range(0, len(jmprel), 24)]
        self.assertEqual(offsets, [DATA_VADDR + 0x18, DATA_VADDR + 0x20])
        rela = self.img.read(self.dyn[p.DT_RELA], self.dyn[p.DT_RELASZ])
        self.assertEqual(p.RELA.unpack(rela), (DATA_VADDR + 0x40, 8, 0x100))

    def test_hash_finds_every_symbol(self):
        for index, name in enumerate(NAMES[1:], start=1):
            self.assertEqual(self.img.lookup(self.dyn, name), index)
        self.assertEqual(self.img.lookup(self.dyn, b"missing"), 0)

    def test_dynamic_segment_read_only(self):
        (dyn,) = [ph for ph in self.img.phdrs if ph.p_type == p.PT_DYNAMIC]
        self.assertEqual(dyn.p_flags, e.PF_R)

    def test_prx_type(self):
        prx = build_elf(e_type=p.ET_SCE_DYNAMIC)
        self.assertEqual(Image(e.convert(prx).data).e_type, p.ET_EXEC)

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
                         (e.SHT_STRTAB, dyn[p.DT_STRTAB], dyn[p.DT_STRSZ]))
        (pt_dyn,) = [ph for ph in img.phdrs if ph.p_type == p.PT_DYNAMIC]
        self.assertEqual((dynamic[1], dynamic[3], dynamic[5], dynamic[9]),
                         (e.SHT_DYNAMIC, pt_dyn.p_vaddr, pt_dyn.p_filesz, p.DYN.size))
        self.assertEqual(img.sections()[dynamic[6]][0], b".dynstr")
        self.assertTrue(new.p_vaddr <= dynstr[3] and dynamic[3] + dynamic[5] <= new.p_vaddr
                        + new.p_filesz)

    def test_read_only_segment_gets_rodata(self):
        # None of the pinned files has one, but a read-only PT_LOAD must still be covered.
        ro = e.Phdr(p.PT_LOAD, e.PF_R, 0x1000, 0x4000, 0x4000, 0x100, 0x180, 0x4000)
        inner = e.Section(b".x", e.SHT_PROGBITS, 0x4040, 0x1040, 0x20, 4)
        got = [(sec.name, sec.sh_type, sec.flags, sec.vaddr, sec.offset, sec.size)
               for sec in e.fill_segments([ro], [inner])]
        self.assertEqual(got, [
            (b".rodata", e.SHT_PROGBITS, e.SHF_ALLOC, 0x4000, 0x1000, 0x40),
            (b".rodata", e.SHT_PROGBITS, e.SHF_ALLOC, 0x4060, 0x1060, 0xA0),
            (b".bss", e.SHT_NOBITS, e.SHF_ALLOC, 0x4100, 0x1100, 0x80),
        ])

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
        with self.assertRaisesRegex(p.ElfError, message):
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
        with self.assertRaisesRegex(p.ElfError, message):
            e.convert(data)

    def test_plain_elf(self):
        self.assertRejects(build_elf(e_type=p.ET_EXEC), "unexpected e_type")

    def test_not_elf(self):
        self.assertRejects(b"\0" * 0x100, "not an ELF")

    def test_missing_tag(self):
        self.assertRejects(build_elf(drop_tags={p.DT_SCE_SYMTAB}), "missing dynamic tag")

    def test_table_out_of_range(self):
        self.assertRejects(build_elf(override={p.DT_SCE_STRSZ: 0x10000}), "past end of dynlibdata")

    def test_rel_instead_of_rela(self):
        self.assertRejects(build_elf(override={p.DT_SCE_PLTREL: 17}), "not RELA")

    def test_standard_tag_clash(self):
        self.assertRejects(build_elf(extra_tags=[(p.DT_STRTAB, 0)]), "unexpected standard tag")

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
        for patch in (mock.patch.object(e, "PINNED_ELFS", known),
                      mock.patch.object(e, "ELF_DIR", self.elf_dir)):
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
        path.write_bytes(build_elf(e_type=p.ET_SCE_DYNAMIC))
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
        self.assertEqual([f.name for f in out.iterdir()], ["synthetic.elf"])

    def test_default_run_reports_missing_pinned_file(self):
        status, err = self.run_main("-o", str(self.tmp / "out"))
        self.assertEqual(status, 1)
        self.assertIn("synthetic.elf", err)


GAME_DIR = load_game_dir()


@unittest.skipUnless(GAME_DIR, "BB_GAME_DIR not set (environment or local.env)")
class GameTests(unittest.TestCase):
    def test_all_pinned_elfs(self):
        readelf = shutil.which("readelf")
        converted = set()
        for path in self2elf.default_inputs(GAME_DIR):
            with self.subTest(path.name):
                data = self2elf.self_to_elf(path.read_bytes()).data
                known = p.PINNED_ELFS.get(hashlib.sha256(data).hexdigest())
                self.assertIsNotNone(known, "rebuilt ELF is not pinned in elf2std")
                converted.add(known)
                result = e.convert(data)
                img = Image(result.data)
                dyn = dict(img.dynamic())
                strtab = dyn[p.DT_STRTAB]
                for index in range(1, result.symbols):
                    sym = p.SYM.unpack(img.read(dyn[p.DT_SYMTAB] + index * p.SYM.size, p.SYM.size))
                    name = img.cstr(strtab + sym[0])
                    # Names are not unique (e.g. a weak and a global entry), so only check
                    # that the lookup returns a symbol with this name.
                    self.assertNotEqual(img.lookup(dyn, name), 0, name)
                # In every pinned file .eh_frame ends with a terminator right before
                # .eh_frame_hdr.
                (eh_hdr,) = [ph for ph in img.phdrs if ph.p_type == p.PT_GNU_EH_FRAME]
                self.assertEqual(sum(result.eh_frame), eh_hdr.p_vaddr)
                self.assertEqual(img.read(eh_hdr.p_vaddr - 4, 4), bytes(4))
                self.assertGreater(result.fdes, 0)
                if readelf:
                    with tempfile.NamedTemporaryFile(suffix=".elf") as tmp:
                        tmp.write(result.data)
                        tmp.flush()
                        # With section headers, readelf finds relocations and dynamic symbols
                        # only with --use-dynamic, and combining that with -r and --dyn-syms in
                        # one run makes it report bad symbol indexes (also without sections).
                        for args in (["-h", "-l", "-S", "-d"], ["-r", "--use-dynamic"],
                                     ["--dyn-syms", "--use-dynamic"]):
                            run = subprocess.run([readelf, *args, "-W", tmp.name],
                                                 capture_output=True, text=True)
                            self.assertEqual((run.returncode, run.stderr), (0, ""), args)
                            if args[0] == "-r":
                                self.assertEqual(run.stdout.count(" R_X86_64_"),
                                                 result.plt_relocs + result.relocs)
        self.assertEqual(converted, set(p.PINNED_ELFS.values()))


if __name__ == "__main__":
    unittest.main()
