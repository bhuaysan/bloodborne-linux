#!/usr/bin/env python3
"""Rewrite a PS4 ELF into a standard ELF that stock tools (Ghidra, readelf, objdump) understand.

Input is an ELF rebuilt by tools/self2elf. PS4 ELFs keep their dynamic linking data in the
PT_SCE_DYNLIBDATA segment, which is not loaded, and describe it with Sony dynamic tags whose
values are offsets into that segment. The tables themselves (string table, Elf64_Sym, Elf64_Rela)
use the standard layouts. This tool:

- sets e_type to ET_EXEC, so tools keep the link addresses (Ghidra rebases ET_DYN images),
- appends a new read-only PT_LOAD segment above the image holding a copy of the dynlibdata
  segment, a SysV hash table and a standard dynamic table pointing into that copy,
- repoints PT_DYNAMIC at the new dynamic table.

All original bytes stay where they are, so addresses in the image are unchanged. Imported symbols
keep their encoded names (NID#library#module). The format is described in
docs/formats/elf-dynamic.md.

Usage:
    elf2std.py                    # every pinned ELF in analysis/elf/
    elf2std.py FILE [FILE ...]    # specific files
    elf2std.py -o DIR ...         # output directory (default: analysis/elf-std/)
"""

from __future__ import annotations

import argparse
import hashlib
import os
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_IN = REPO_ROOT / "analysis" / "elf"
DEFAULT_OUT = REPO_ROOT / "analysis" / "elf-std"

# ELFs rebuilt by self2elf from CUSA03173 patch 1.09 (eboot.bin and sce_module/*.prx).
KNOWN_FILES = {
    "941f887a562aae054fac35af8cc8f27cf075f3d4cc2e029fb5ae2a663aaa5ae7": "eboot.elf",
    "9190c68ea3e1f5cec45c9ec98c9b3b5963241a213bce357c956e82477779d5e9": "libc.elf",
    "304c9c4ecb7fe2ff9cf59248bdd6e369e67d74d724b5cd7d18c0a82dd8e5f82c": "libSceFace.elf",
    "8d52cf49b87b8127a071d8a66f12af7e49277bcde6c8c87d4a0a59aab8211f32": "libSceFios2.elf",
    "cdd2479c6e718712892bc631e124a56668cf20504279251d7b3a62ddfc7eaef2": "libSceHand.elf",
    "9e872ba87d25adf863d641eb5600590e7844197384cb5c343e3853bee9cc7436": "libSceHeadTracker.elf",
    "871e3848cd09ded5dbf76dcd16a26e8887b8c50bb424c2ef18fa0a237acd502f": "libSceS3DConversion.elf",
    "12a7735255ae17a7c3765674be39e841ab7cdfb54854bbc5e37334bd049a8553": "libSceSmart.elf",
}

ELF_HEADER = struct.Struct("<16sHHIQQQIHHHHHH")  # 0x40 bytes
ELF_PHDR = struct.Struct("<IIQQQQQQ")  # 0x38 bytes
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
PF_R = 4
SEGMENT_ALIGN = 0x4000

DT_NULL = 0
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
DT_PLTREL = 20
DT_JMPREL = 23
DT_LOOS = 0x6000000D

# Sony dynamic tags (our names; the values were matched against the table layouts in the files).
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

# Sony tag -> standard tag. Offsets are relative to the dynlibdata segment.
OFFSET_TAGS = {
    DT_SCE_JMPREL: DT_JMPREL,
    DT_SCE_RELA: DT_RELA,
    DT_SCE_STRTAB: DT_STRTAB,
    DT_SCE_SYMTAB: DT_SYMTAB,
}
VALUE_TAGS = {
    DT_SCE_PLTGOT: DT_PLTGOT,  # already a virtual address
    DT_SCE_PLTREL: DT_PLTREL,
    DT_SCE_PLTRELSZ: DT_PLTRELSZ,
    DT_SCE_RELASZ: DT_RELASZ,
    DT_SCE_RELAENT: DT_RELAENT,
    DT_SCE_STRSZ: DT_STRSZ,
    DT_SCE_SYMENT: DT_SYMENT,
}
REQUIRED_TAGS = list(OFFSET_TAGS) + list(VALUE_TAGS) + [DT_SCE_SYMTABSZ]


class ElfError(Exception):
    pass


@dataclass
class Phdr:
    p_type: int
    p_flags: int
    p_offset: int
    p_vaddr: int
    p_paddr: int
    p_filesz: int
    p_memsz: int
    p_align: int

    def pack(self) -> bytes:
        return ELF_PHDR.pack(self.p_type, self.p_flags, self.p_offset, self.p_vaddr,
                             self.p_paddr, self.p_filesz, self.p_memsz, self.p_align)


@dataclass
class Result:
    data: bytes
    load_vaddr: int  # where the dynlibdata copy is mapped
    symbols: int
    plt_relocs: int
    relocs: int


def align_up(value: int, alignment: int) -> int:
    return (value + alignment - 1) // alignment * alignment


def sysv_hash(name: bytes) -> int:
    """The ELF hash function from the System V ABI."""
    h = 0
    for c in name:
        h = ((h << 4) + c) & 0xFFFFFFFF
        g = h & 0xF0000000
        if g:
            h ^= g >> 24
        h &= ~g & 0xFFFFFFFF
    return h


def build_hash_table(names: list[bytes]) -> bytes:
    """SysV DT_HASH table for the given symbol names (index 0 is the null symbol)."""
    nchain = len(names)
    nbucket = max(1, nchain // 4)
    buckets = [0] * nbucket
    chains = [0] * nchain
    for index in range(1, nchain):
        slot = sysv_hash(names[index]) % nbucket
        # Prepend to the bucket's chain. Lookups walk the chain, so order doesn't matter.
        chains[index] = buckets[slot]
        buckets[slot] = index
    return struct.pack(f"<II{nbucket}I{nchain}I", nbucket, nchain, *buckets, *chains)


def _need(cond: bool, what: str) -> None:
    if not cond:
        raise ElfError(what)


def convert(data: bytes) -> Result:
    _need(len(data) >= ELF_HEADER.size, "file too small for an ELF header")
    (ident, e_type, e_machine, e_version, e_entry, e_phoff, e_shoff, e_flags, e_ehsize,
     e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx) = ELF_HEADER.unpack_from(data)
    _need(ident[:4] == b"\x7fELF", "not an ELF file")
    _need(ident[4] == 2 and ident[5] == 1, "not a 64-bit little-endian ELF")
    _need(e_type in (ET_SCE_DYNEXEC, ET_SCE_DYNAMIC), f"unexpected e_type {e_type:#x}")
    _need(e_machine == EM_X86_64, f"unexpected e_machine {e_machine}")
    _need(e_phentsize == ELF_PHDR.size, "unexpected program header size")
    _need(e_shnum == 0, "ELF already has section headers")

    phdrs_end = e_phoff + e_phnum * ELF_PHDR.size
    _need(phdrs_end <= len(data), "program headers past end of file")
    phdrs = [Phdr(*ELF_PHDR.unpack_from(data, e_phoff + i * ELF_PHDR.size)) for i in range(e_phnum)]
    for ph in phdrs:
        _need(ph.p_offset + ph.p_filesz <= len(data), f"segment type {ph.p_type:#x} past end of file")

    def single(p_type: int, what: str) -> Phdr:
        found = [ph for ph in phdrs if ph.p_type == p_type]
        _need(len(found) == 1, f"expected one {what} segment, found {len(found)}")
        return found[0]

    dynamic = single(PT_DYNAMIC, "PT_DYNAMIC")
    dynlib = single(PT_SCE_DYNLIBDATA, "PT_SCE_DYNLIBDATA")
    loads = [ph for ph in phdrs if ph.p_type == PT_LOAD]
    _need(bool(loads), "no PT_LOAD segment")

    # The new program header goes into the gap after the existing ones.
    file_start = min(ph.p_offset for ph in phdrs if ph.p_filesz)
    new_phdrs_end = phdrs_end + ELF_PHDR.size
    _need(new_phdrs_end <= file_start and not any(data[phdrs_end:new_phdrs_end]),
          "no free space for an extra program header")

    # Dynamic entries, up to DT_NULL.
    entries: list[tuple[int, int]] = []
    for off in range(dynamic.p_offset, dynamic.p_offset + dynamic.p_filesz - DYN.size + 1, DYN.size):
        tag, value = DYN.unpack_from(data, off)
        if tag == DT_NULL:
            break
        entries.append((tag, value))
    else:
        raise ElfError("dynamic table has no DT_NULL")

    sce: dict[int, int] = {}
    for tag, value in entries:
        if tag in REQUIRED_TAGS + [DT_SCE_HASH, DT_SCE_HASHSZ]:
            _need(tag not in sce, f"duplicate dynamic tag {tag:#x}")
            sce[tag] = value
    for tag in REQUIRED_TAGS:
        _need(tag in sce, f"missing dynamic tag {tag:#x}")
    _need(sce[DT_SCE_PLTREL] == DT_RELA, "PLT relocations are not RELA")
    _need(sce[DT_SCE_SYMENT] == SYM.size, "unexpected symbol entry size")
    _need(sce[DT_SCE_RELAENT] == RELA_SIZE, "unexpected relocation entry size")

    blob = bytearray(data[dynlib.p_offset: dynlib.p_offset + dynlib.p_filesz])

    def table(offset_tag: int, size: int, what: str) -> tuple[int, int]:
        start = sce[offset_tag]
        _need(start + size <= len(blob), f"{what} past end of dynlibdata")
        return start, size

    str_off, str_size = table(DT_SCE_STRTAB, sce[DT_SCE_STRSZ], "string table")
    sym_off, sym_size = table(DT_SCE_SYMTAB, sce[DT_SCE_SYMTABSZ], "symbol table")
    table(DT_SCE_JMPREL, sce[DT_SCE_PLTRELSZ], "PLT relocations")
    table(DT_SCE_RELA, sce[DT_SCE_RELASZ], "relocations")
    _need(sym_size % SYM.size == 0, "symbol table size not a multiple of the entry size")
    _need(sce[DT_SCE_PLTRELSZ] % RELA_SIZE == 0 and sce[DT_SCE_RELASZ] % RELA_SIZE == 0,
          "relocation table size not a multiple of the entry size")

    names = []
    for off in range(sym_off, sym_off + sym_size, SYM.size):
        name_off = SYM.unpack_from(blob, off)[0]
        _need(name_off < str_size, "symbol name outside the string table")
        end = blob.find(b"\0", str_off + name_off, str_off + str_size)
        _need(end >= 0, "unterminated symbol name")
        names.append(bytes(blob[str_off + name_off: end]))

    # Layout of the new segment: [dynlibdata copy][hash table][dynamic table]
    load_vaddr = align_up(max(ph.p_vaddr + ph.p_memsz for ph in loads), SEGMENT_ALIGN)
    load_offset = align_up(len(data), SEGMENT_ALIGN)
    blob += bytes(align_up(len(blob), 8) - len(blob))
    hash_off = len(blob)
    blob += build_hash_table(names)
    blob += bytes(align_up(len(blob), 16) - len(blob))
    dyn_off = len(blob)

    new_entries = []
    for tag, value in entries:
        # Standard tags (DT_NEEDED, DT_SONAME, DT_INIT, DT_INIT_ARRAY, ...) already hold string
        # offsets or virtual addresses and are kept. The other Sony tags describe modules and
        # libraries; they remain readable in the dynlibdata copy.
        if tag < DT_LOOS:
            _need(tag not in OFFSET_TAGS.values() and tag not in VALUE_TAGS.values()
                  and tag != DT_HASH, f"unexpected standard tag {tag:#x}")
            new_entries.append((tag, value))
    new_entries.append((DT_HASH, load_vaddr + hash_off))
    for sce_tag, std_tag in OFFSET_TAGS.items():
        new_entries.append((std_tag, load_vaddr + sce[sce_tag]))
    for sce_tag, std_tag in VALUE_TAGS.items():
        new_entries.append((std_tag, sce[sce_tag]))
    new_entries.append((DT_NULL, 0))
    for tag, value in new_entries:
        blob += DYN.pack(tag, value)
    dyn_size = len(new_entries) * DYN.size

    out = bytearray(data)
    out += bytes(load_offset - len(out))
    out += blob

    header = ELF_HEADER.pack(ident, ET_EXEC, e_machine, e_version, e_entry, e_phoff, e_shoff,
                             e_flags, e_ehsize, e_phentsize, e_phnum + 1, e_shentsize, e_shnum,
                             e_shstrndx)
    out[:ELF_HEADER.size] = header
    for i, ph in enumerate(phdrs):
        if ph is dynamic:
            ph = Phdr(PT_DYNAMIC, ph.p_flags, load_offset + dyn_off, load_vaddr + dyn_off,
                      load_vaddr + dyn_off, dyn_size, dyn_size, ph.p_align)
        out[e_phoff + i * ELF_PHDR.size: e_phoff + (i + 1) * ELF_PHDR.size] = ph.pack()
    new_load = Phdr(PT_LOAD, PF_R, load_offset, load_vaddr, load_vaddr, len(blob), len(blob),
                    SEGMENT_ALIGN)
    out[phdrs_end:new_phdrs_end] = new_load.pack()

    return Result(bytes(out), load_vaddr, len(names), sce[DT_SCE_PLTRELSZ] // RELA_SIZE,
                  sce[DT_SCE_RELASZ] // RELA_SIZE)


def load_game_dir() -> Path | None:
    """BB_GAME_DIR from the environment or local.env in the repo root."""
    value = os.environ.get("BB_GAME_DIR")
    env_file = REPO_ROOT / "local.env"
    if not value and env_file.is_file():
        for line in env_file.read_text().splitlines():
            key, sep, rest = line.partition("=")
            if sep and key.removeprefix("export ").strip() == "BB_GAME_DIR":
                value = rest.strip().strip("\"'")
    return Path(value).expanduser().resolve() if value else None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("files", nargs="*", type=Path,
                        help="ELF files from self2elf (default: every pinned ELF in analysis/elf/)")
    parser.add_argument("-o", "--out-dir", type=Path, default=DEFAULT_OUT,
                        help="output directory (default: analysis/elf-std/ in the repo)")
    args = parser.parse_args(argv)

    files = args.files or sorted(p for p in DEFAULT_IN.glob("*.elf"))
    if not files:
        parser.error(f"no files given and none found in {DEFAULT_IN} (run self2elf first)")
    out_dir = args.out_dir.expanduser().resolve()
    game_dir = load_game_dir()
    if game_dir is not None and out_dir.is_relative_to(game_dir):
        parser.error("refusing to write into the game directory")

    status = 0
    for path in files:
        data = path.read_bytes()
        known = KNOWN_FILES.get(hashlib.sha256(data).hexdigest())
        if known is None:
            print(f"{path}: SHA-256 is not a pinned self2elf output, skipped", file=sys.stderr)
            status = 1
            continue
        if path.resolve().parent == out_dir:
            print(f"{path}: refusing to write next to the input", file=sys.stderr)
            status = 1
            continue
        try:
            result = convert(data)
        except ElfError as exc:
            print(f"{path}: {exc}", file=sys.stderr)
            status = 1
            continue
        out_dir.mkdir(parents=True, exist_ok=True)
        out_path = out_dir / known
        out_path.write_bytes(result.data)
        print(f"{known}: symbols={result.symbols} plt_relocs={result.plt_relocs} "
              f"relocs={result.relocs} dynlibdata@{result.load_vaddr:#x} -> {out_path}")
    return status


if __name__ == "__main__":
    sys.exit(main())
