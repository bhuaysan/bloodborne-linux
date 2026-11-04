"""Reader for the dynamic linking data of PS4 ELFs rebuilt by tools/self2elf.

PS4 ELFs keep their string, symbol and relocation tables in the PT_SCE_DYNLIBDATA segment, which
is not loaded. The dynamic table refers to them with Sony tags whose values are offsets into that
segment. The tables use the standard layouts. See docs/formats/elf-dynamic.md.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from functools import cached_property

from toolenv import ELF_HEADER, ELF_PHDR, Phdr

# ELFs rebuilt by self2elf from CUSA03173 patch 1.09 (eboot.bin and sce_module/*.prx).
PINNED_ELFS = {
    "941f887a562aae054fac35af8cc8f27cf075f3d4cc2e029fb5ae2a663aaa5ae7": "eboot.elf",
    "9190c68ea3e1f5cec45c9ec98c9b3b5963241a213bce357c956e82477779d5e9": "libc.elf",
    "304c9c4ecb7fe2ff9cf59248bdd6e369e67d74d724b5cd7d18c0a82dd8e5f82c": "libSceFace.elf",
    "8d52cf49b87b8127a071d8a66f12af7e49277bcde6c8c87d4a0a59aab8211f32": "libSceFios2.elf",
    "cdd2479c6e718712892bc631e124a56668cf20504279251d7b3a62ddfc7eaef2": "libSceHand.elf",
    "9e872ba87d25adf863d641eb5600590e7844197384cb5c343e3853bee9cc7436": "libSceHeadTracker.elf",
    "871e3848cd09ded5dbf76dcd16a26e8887b8c50bb424c2ef18fa0a237acd502f": "libSceS3DConversion.elf",
    "12a7735255ae17a7c3765674be39e841ab7cdfb54854bbc5e37334bd049a8553": "libSceSmart.elf",
}

# Error hint for files that are not one of these. The usual cause is a stale or edited output.
PINNED_ELFS_HINT = "expected a self2elf output of CUSA03173 patch 1.09, run tools/self2elf again"

DYN = struct.Struct("<QQ")  # 0x10 bytes
SYM = struct.Struct("<IBBHQQ")  # 0x18 bytes
RELA_SIZE = 0x18

ET_EXEC = 2
ET_SCE_DYNEXEC = 0xFE10  # eboot.bin
ET_SCE_DYNAMIC = 0xFE18  # .prx
EM_X86_64 = 62

PT_LOAD = 1
PT_DYNAMIC = 2
PT_SCE_DYNLIBDATA = 0x61000000

DT_NULL = 0
DT_NEEDED = 1
DT_PLTRELSZ = 2
DT_PLTGOT = 3
DT_HASH = 4
DT_STRTAB = 5
DT_SYMTAB = 6
DT_RELA = 7
DT_RELASZ = 8
DT_RELAENT = 9
DT_STRSZ = 10
DT_SYMENT = 11
DT_SONAME = 14
DT_PLTREL = 20
DT_JMPREL = 23
DT_LOOS = 0x6000000D

# Sony dynamic tags (our names; the values were matched against the table layouts in the files).
# Every Sony tag seen so far is 0x610000xx. We treat 0x61xxxxxx as the Sony range.
DT_SCE_FIRST = 0x61000000
DT_SCE_LAST = 0x61FFFFFF
DT_SCE_MODULE_INFO = 0x6100000D  # this file's module
DT_SCE_NEEDED_MODULE = 0x6100000F
DT_SCE_EXPORT_LIB = 0x61000013
DT_SCE_IMPORT_LIB = 0x61000015
DT_SCE_EXPORT_LIB_ATTR = 0x61000017
DT_SCE_IMPORT_LIB_ATTR = 0x61000019
DT_SCE_HASH = 0x61000025
DT_SCE_PLTGOT = 0x61000027
DT_SCE_JMPREL = 0x61000029
DT_SCE_PLTREL = 0x6100002B
DT_SCE_PLTRELSZ = 0x6100002D
DT_SCE_RELA = 0x6100002F
DT_SCE_RELASZ = 0x61000031
DT_SCE_RELAENT = 0x61000033
DT_SCE_STRTAB = 0x61000035
DT_SCE_STRSZ = 0x61000037
DT_SCE_SYMTAB = 0x61000039
DT_SCE_SYMENT = 0x6100003B
DT_SCE_HASHSZ = 0x6100003D
DT_SCE_SYMTABSZ = 0x6100003F

# Tags that locate the tables. Each must appear exactly once.
REQUIRED_TAGS = frozenset({
    DT_SCE_PLTGOT, DT_SCE_JMPREL, DT_SCE_PLTREL, DT_SCE_PLTRELSZ, DT_SCE_RELA, DT_SCE_RELASZ,
    DT_SCE_RELAENT, DT_SCE_STRTAB, DT_SCE_STRSZ, DT_SCE_SYMTAB, DT_SCE_SYMENT, DT_SCE_SYMTABSZ,
})
TABLE_TAGS = REQUIRED_TAGS | {DT_SCE_HASH, DT_SCE_HASHSZ}


class ElfError(Exception):
    pass


def need(cond: bool, what: str) -> None:
    if not cond:
        raise ElfError(what)


@dataclass
class Symbol:
    name: bytes
    info: int
    other: int
    shndx: int
    value: int
    size: int

    @property
    def bind(self) -> int:
        return self.info >> 4

    @property
    def type(self) -> int:
        return self.info & 0xF


@dataclass
class DynImage:
    """A parsed PS4 ELF: header, program headers, dynamic entries and the dynlibdata segment."""

    data: bytes
    header: tuple  # ELF_HEADER fields
    phdrs: list[Phdr]
    dynamic: Phdr
    dynlib: Phdr
    entries: list[tuple[int, int]]  # dynamic entries up to DT_NULL, in file order
    tables: dict[int, int]  # TABLE_TAGS values
    blob: bytes  # contents of the dynlibdata segment

    @property
    def e_type(self) -> int:
        return self.header[1]

    def string(self, offset: int) -> bytes:
        """NUL-terminated string at an offset into the string table."""
        str_off, str_size = self.tables[DT_SCE_STRTAB], self.tables[DT_SCE_STRSZ]
        need(offset < str_size, "string offset outside the string table")
        end = self.blob.find(b"\0", str_off + offset, str_off + str_size)
        need(end >= 0, "unterminated string")
        return self.blob[str_off + offset: end]

    @cached_property
    def symbols(self) -> list[Symbol]:
        sym_off, sym_size = self.tables[DT_SCE_SYMTAB], self.tables[DT_SCE_SYMTABSZ]
        out = []
        for off in range(sym_off, sym_off + sym_size, SYM.size):
            name_off, info, other, shndx, value, size = SYM.unpack_from(self.blob, off)
            out.append(Symbol(self.string(name_off), info, other, shndx, value, size))
        return out


def parse(data: bytes) -> DynImage:
    need(len(data) >= ELF_HEADER.size, "file too small for an ELF header")
    header = ELF_HEADER.unpack_from(data)
    ident, e_type, e_machine = header[0], header[1], header[2]
    e_phoff, e_phentsize, e_phnum = header[5], header[9], header[10]
    need(ident[:4] == b"\x7fELF", "not an ELF file")
    need(ident[4] == 2 and ident[5] == 1, "not a 64-bit little-endian ELF")
    need(e_type in (ET_SCE_DYNEXEC, ET_SCE_DYNAMIC), f"unexpected e_type {e_type:#x}")
    need(e_machine == EM_X86_64, f"unexpected e_machine {e_machine}")
    need(e_phentsize == ELF_PHDR.size, "unexpected program header size")

    need(e_phoff + e_phnum * ELF_PHDR.size <= len(data), "program headers past end of file")
    phdrs = [Phdr(*ELF_PHDR.unpack_from(data, e_phoff + i * ELF_PHDR.size)) for i in range(e_phnum)]
    for ph in phdrs:
        need(ph.p_offset + ph.p_filesz <= len(data), f"segment type {ph.p_type:#x} past end of file")

    def single(p_type: int, what: str) -> Phdr:
        found = [ph for ph in phdrs if ph.p_type == p_type]
        need(len(found) == 1, f"expected one {what} segment, found {len(found)}")
        return found[0]

    dynamic = single(PT_DYNAMIC, "PT_DYNAMIC")
    dynlib = single(PT_SCE_DYNLIBDATA, "PT_SCE_DYNLIBDATA")

    entries: list[tuple[int, int]] = []
    for off in range(dynamic.p_offset, dynamic.p_offset + dynamic.p_filesz - DYN.size + 1, DYN.size):
        tag, value = DYN.unpack_from(data, off)
        if tag == DT_NULL:
            break
        entries.append((tag, value))
    else:
        raise ElfError("dynamic table has no DT_NULL")

    tables: dict[int, int] = {}
    for tag, value in entries:
        if tag in TABLE_TAGS:
            need(tag not in tables, f"duplicate dynamic tag {tag:#x}")
            tables[tag] = value
    for tag in sorted(REQUIRED_TAGS):
        need(tag in tables, f"missing dynamic tag {tag:#x}")
    need(tables[DT_SCE_PLTREL] == DT_RELA, "PLT relocations are not RELA")
    need(tables[DT_SCE_SYMENT] == SYM.size, "unexpected symbol entry size")
    need(tables[DT_SCE_RELAENT] == RELA_SIZE, "unexpected relocation entry size")

    blob = data[dynlib.p_offset: dynlib.p_offset + dynlib.p_filesz]
    for offset_tag, size_tag, what in ((DT_SCE_STRTAB, DT_SCE_STRSZ, "string table"),
                                       (DT_SCE_SYMTAB, DT_SCE_SYMTABSZ, "symbol table"),
                                       (DT_SCE_JMPREL, DT_SCE_PLTRELSZ, "PLT relocations"),
                                       (DT_SCE_RELA, DT_SCE_RELASZ, "relocations")):
        need(tables[offset_tag] + tables[size_tag] <= len(blob), f"{what} past end of dynlibdata")
    need(tables[DT_SCE_SYMTABSZ] % SYM.size == 0, "symbol table size not a multiple of the entry size")
    need(tables[DT_SCE_PLTRELSZ] % RELA_SIZE == 0 and tables[DT_SCE_RELASZ] % RELA_SIZE == 0,
         "relocation table size not a multiple of the entry size")

    image = DynImage(data, header, phdrs, dynamic, dynlib, entries, tables, blob)
    image.symbols  # validates every symbol name up front
    return image
