"""Builds minimal PS4 ELFs for the tool tests. Not used by the tools themselves."""

from __future__ import annotations

import ps4elf as p

NOTYPE, FUNC, OBJECT, SECTION = 0, 2, 1, 3
GLOBAL, WEAK, LOCAL = 1, 2, 0


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
            p.DT_SCE_RELAENT: p.RELA.size, p.DT_SCE_STRTAB: str_off,
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
