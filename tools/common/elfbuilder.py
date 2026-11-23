"""Builds minimal PS4 ELFs for the tool tests. Not used by the tools themselves."""

from __future__ import annotations

import struct

import ps4elf as p

NOTYPE, FUNC, OBJECT, SECTION = 0, 2, 1, 3
GLOBAL, WEAK, LOCAL = 1, 2, 0
R_X86_64_RELATIVE, R_X86_64_JUMP_SLOT = 8, 7

PAGE = 0x1000


class Builder:
    """A minimal PS4 ELF: a dynlibdata segment with records, symbols and relocations.

    Without load() calls the file has one placeholder PT_LOAD over its first 0x100 bytes.
    Otherwise every load() adds a PT_LOAD segment at the next page of the file, in call
    order, and dynlibdata follows the last one."""

    def __init__(self, prx: bool = False, module: str = "main", soname: str | None = None,
                 e_type: int | None = None, entry: int = 0):
        self.prx = prx
        self.e_type = e_type
        self.entry = entry
        self.strtab = bytearray(b"\0")
        self.entries: list[tuple[int, int]] = []
        self.symbols: list[tuple[str, int, int, bool]] = []
        self.plt_relocs: list[bytes] = []
        self.relocs: list[bytes] = []
        self.overrides: dict[int, int | None] = {}
        self.hash_size = 0
        self.loads: list[tuple[int, int, bytearray, int]] = []  # flags, vaddr, data, memsz
        self.eh_hdr: tuple[int, int] | None = None
        if soname:
            self.entries.append((p.DT_SONAME, self.str(soname)))
        self.record(p.DT_SCE_MODULE_INFO, 0, 0x0101, module)

    def str(self, text: str) -> int:
        off = len(self.strtab)
        self.strtab += text.encode() + b"\0"
        return off

    def tag(self, tag: int, value: int) -> Builder:
        """A raw dynamic entry, before the table tags."""
        self.entries.append((tag, value))
        return self

    def table(self, tag: int, value: int | None) -> Builder:
        """Overrides a table tag that build() computes (DT_SCE_STRSZ, ...); None drops it."""
        self.overrides[tag] = value
        return self

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

    def jump_slot(self, offset: int, sym: int) -> Builder:
        """A PLT relocation (DT_SCE_JMPREL) of symbol index sym at offset."""
        self.plt_relocs.append(p.RELA.pack(offset, sym << 32 | R_X86_64_JUMP_SLOT, 0))
        return self

    def rela(self, offset: int, rtype: int, sym: int = 0, addend: int = 0) -> Builder:
        """A relocation in DT_SCE_RELA."""
        self.relocs.append(p.RELA.pack(offset, sym << 32 | rtype, addend))
        return self

    def sony_hash(self, size: int = 0x10) -> Builder:
        """A zero-filled DT_SCE_HASH table of size bytes."""
        self.hash_size = size
        return self

    def load(self, vaddr: int, data: bytes, memsz: int | None = None,
             flags: int = p.PF_R | p.PF_X) -> Builder:
        self.loads.append((flags, vaddr, bytearray(data), len(data) if memsz is None else memsz))
        return self

    def place(self, vaddr: int, data: bytes) -> Builder:
        """Overwrites bytes of the load segment that holds [vaddr, vaddr + len(data))."""
        for _, start, seg, _ in self.loads:
            if start <= vaddr and vaddr + len(data) <= start + len(seg):
                seg[vaddr - start: vaddr - start + len(data)] = data
                return self
        raise ValueError(f"{vaddr:#x}+{len(data):#x} is not in a load segment's file data")

    def eh_frame_hdr(self, vaddr: int, data: bytes) -> Builder:
        """Places data (see build_eh_frame_hdr) at vaddr and describes it by PT_GNU_EH_FRAME."""
        self.place(vaddr, data)
        self.eh_hdr = (vaddr, len(data))
        return self

    def dynlib(self) -> tuple[bytes, int]:
        """The dynlibdata segment and the offset of the dynamic table inside it."""
        strtab = bytearray(self.strtab)
        syms = [p.SYM.pack(0, 0, 0, 0, 0, 0)]
        for index, (name, typ, bind, defined) in enumerate(self.symbols, start=1):
            name_off = 0
            if name:
                name_off = len(strtab)
                strtab += name.encode() + b"\0"
            syms.append(p.SYM.pack(name_off, bind << 4 | typ, 0, 1 if defined else 0,
                                   0x1000 + index * 0x10 if defined else 0, 0))
        symtab = b"".join(syms)
        blob = bytearray(bytes(0x10))  # leading padding, so offsets are non-zero

        def put(chunk: bytes) -> int:
            off = len(blob)
            blob.extend(chunk)
            return off
        hash_off = put(bytes(self.hash_size))
        str_off = put(strtab)
        while len(blob) % 8:
            blob += b"\0"
        sym_off = put(symtab)
        jmprel, rela = b"".join(self.plt_relocs), b"".join(self.relocs)
        jmp_off = put(jmprel) if jmprel else sym_off
        rela_off = put(rela) if rela else sym_off
        tables = {
            p.DT_SCE_PLTGOT: 0, p.DT_SCE_JMPREL: jmp_off, p.DT_SCE_PLTREL: p.DT_RELA,
            p.DT_SCE_PLTRELSZ: len(jmprel), p.DT_SCE_RELA: rela_off, p.DT_SCE_RELASZ: len(rela),
            p.DT_SCE_RELAENT: p.RELA.size, p.DT_SCE_STRTAB: str_off,
            p.DT_SCE_STRSZ: len(strtab), p.DT_SCE_SYMTAB: sym_off,
            p.DT_SCE_SYMENT: p.SYM.size, p.DT_SCE_SYMTABSZ: len(symtab),
        }
        if self.hash_size:
            tables = {p.DT_SCE_HASH: hash_off, p.DT_SCE_HASHSZ: self.hash_size, **tables}
        tables.update(self.overrides)
        dyn_off = len(blob)
        for tag, value in [*self.entries, *tables.items(), (p.DT_NULL, 0)]:
            if value is not None:
                blob += p.DYN.pack(tag, value)
        return bytes(blob), dyn_off

    def build(self) -> bytes:
        blob, dyn_off = self.dynlib()
        phoff = 0x40
        if self.loads:
            phdrs, offset, segs = [], PAGE, []
            for flags, vaddr, seg, memsz in self.loads:
                phdrs.append(p.Phdr(p.PT_LOAD, flags, offset, vaddr, vaddr, len(seg), memsz,
                                    0x4000))
                segs.append((offset, seg))
                offset += -(-max(len(seg), 1) // PAGE) * PAGE
            dynlib_off = offset
        else:
            phdrs, segs = [p.Phdr(p.PT_LOAD, 5, 0, 0, 0, 0x100, 0x100, 0x4000)], []
            dynlib_off = PAGE
        phdrs += [
            p.Phdr(p.PT_DYNAMIC, 6, dynlib_off + dyn_off, 0, 0, len(blob) - dyn_off,
                   len(blob) - dyn_off, 8),
            p.Phdr(p.PT_SCE_DYNLIBDATA, 4, dynlib_off, 0, 0, len(blob), 0, 0x10),
        ]
        if self.eh_hdr:
            vaddr, size = self.eh_hdr
            (seg,) = [ph for ph in phdrs[:len(self.loads)]
                      if ph.p_vaddr <= vaddr and vaddr + size <= ph.p_vaddr + ph.p_filesz]
            phdrs.append(p.Phdr(p.PT_GNU_EH_FRAME, 4, seg.p_offset + vaddr - seg.p_vaddr, vaddr,
                                vaddr, size, size, 4))
        out = bytearray(dynlib_off + len(blob))
        e_type = self.e_type
        if e_type is None:
            e_type = p.ET_SCE_DYNAMIC if self.prx else p.ET_SCE_DYNEXEC
        ident = b"\x7fELF\x02\x01\x01\x09" + bytes(8)
        out[:0x40] = p.ELF_HEADER.pack(ident, e_type, p.EM_X86_64, 1, self.entry, phoff, 0, 0,
                                       0x40, p.ELF_PHDR.size, len(phdrs), 0x40, 0, 0)
        for i, ph in enumerate(phdrs):
            out[phoff + i * p.ELF_PHDR.size: phoff + (i + 1) * p.ELF_PHDR.size] = ph.pack()
        for offset, seg in segs:
            out[offset: offset + len(seg)] = seg
        out[dynlib_off:] = blob
        return bytes(out)


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
