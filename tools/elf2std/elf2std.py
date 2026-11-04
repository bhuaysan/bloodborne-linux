#!/usr/bin/env python3
"""Rewrite a PS4 ELF into a standard ELF that stock tools (Ghidra, readelf, objdump) understand.

Input is an ELF rebuilt by tools/self2elf. PS4 ELFs keep their dynamic linking data in the
PT_SCE_DYNLIBDATA segment, which is not loaded, and describe it with Sony dynamic tags whose
values are offsets into that segment. The tables themselves (string table, Elf64_Sym, Elf64_Rela)
use the standard layouts. This tool:

- sets e_type to ET_EXEC, so tools keep the link addresses (Ghidra rebases ET_DYN images, and
  REA cannot pass Ghidra's image base option),
- appends a new read-only PT_LOAD segment above the image holding a copy of the dynlibdata
  segment, a SysV hash table and a standard dynamic table pointing into that copy,
- repoints PT_DYNAMIC at the new dynamic table.

All original bytes stay where they are, so addresses in the image are unchanged. Imported symbols
keep their encoded names (NID#library#module). The format is described in
docs/formats/elf-dynamic.md.

Usage:
    elf2std.py                    # the 8 pinned ELFs in analysis/elf/
    elf2std.py FILE [FILE ...]    # specific files
    elf2std.py -o DIR ...         # output directory (default: analysis/elf-std/)

analysis/ is the one in the main checkout, also when run from a linked git worktree.
"""

from __future__ import annotations

import argparse
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "common"))
from toolenv import (  # noqa: E402
    ELF_HEADER, ELF_PHDR, Phdr, PinnedError, analysis_dir, is_inside, load_game_dir, read_pinned,
)
from ps4elf import (  # noqa: E402,F401  (re-exported for the tests)
    DT_HASH, DT_JMPREL, DT_LOOS, DT_NULL, DT_PLTGOT, DT_PLTREL, DT_PLTRELSZ, DT_RELA, DT_RELAENT,
    DT_RELASZ, DT_SCE_FIRST, DT_SCE_HASH, DT_SCE_HASHSZ, DT_SCE_JMPREL, DT_SCE_LAST, DT_SCE_PLTGOT,
    DT_SCE_PLTREL, DT_SCE_PLTRELSZ, DT_SCE_RELA, DT_SCE_RELAENT, DT_SCE_RELASZ, DT_SCE_STRSZ,
    DT_SCE_STRTAB, DT_SCE_SYMENT, DT_SCE_SYMTAB, DT_SCE_SYMTABSZ, DT_STRSZ, DT_STRTAB, DT_SYMENT,
    DT_SYMTAB, DYN, EM_X86_64, ET_EXEC, ET_SCE_DYNAMIC, ET_SCE_DYNEXEC, PINNED_ELFS,
    PINNED_ELFS_HINT, PT_DYNAMIC, PT_LOAD, PT_SCE_DYNLIBDATA, RELA_SIZE, SYM, ElfError, need, parse,
)

DEFAULT_IN = analysis_dir() / "elf"
DEFAULT_OUT = analysis_dir() / "elf-std"

KNOWN_FILES = PINNED_ELFS

PF_R = 4
SEGMENT_ALIGN = 0x4000

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
REPLACED_TAGS = frozenset(OFFSET_TAGS.values()) | frozenset(VALUE_TAGS.values()) | {DT_HASH}


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


def convert(data: bytes) -> Result:
    image = parse(data)
    (ident, _, e_machine, e_version, e_entry, e_phoff, e_shoff, e_flags, e_ehsize,
     e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx) = image.header
    need(e_shnum == 0, "ELF already has section headers")
    phdrs, dynamic, sce = image.phdrs, image.dynamic, image.tables
    loads = [ph for ph in phdrs if ph.p_type == PT_LOAD]
    need(bool(loads), "no PT_LOAD segment")

    # The new program header goes into the gap after the existing ones.
    phdrs_end = e_phoff + e_phnum * ELF_PHDR.size
    file_start = min(ph.p_offset for ph in phdrs if ph.p_filesz)
    new_phdrs_end = phdrs_end + ELF_PHDR.size
    need(new_phdrs_end <= file_start and not any(data[phdrs_end:new_phdrs_end]),
          "no free space for an extra program header")

    entries = image.entries
    blob = bytearray(image.blob)
    names = [sym.name for sym in image.symbols]

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
        # offsets or virtual addresses and are kept. Sony tags are dropped: the ones above are
        # replaced by standard tags, the others describe modules and libraries and remain
        # readable in the dynlibdata copy. Any other OS or processor tag is refused, because we
        # can't tell whether its value would need translating. None of the pinned files has one.
        if tag < DT_LOOS:
            need(tag not in REPLACED_TAGS, f"unexpected standard tag {tag:#x}")
            new_entries.append((tag, value))
        else:
            need(DT_SCE_FIRST <= tag <= DT_SCE_LAST, f"unsupported dynamic tag {tag:#x}")
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
            # Read-only, like the PT_LOAD it now lies in.
            ph = Phdr(PT_DYNAMIC, PF_R, load_offset + dyn_off, load_vaddr + dyn_off,
                      load_vaddr + dyn_off, dyn_size, dyn_size, ph.p_align)
        out[e_phoff + i * ELF_PHDR.size: e_phoff + (i + 1) * ELF_PHDR.size] = ph.pack()
    new_load = Phdr(PT_LOAD, PF_R, load_offset, load_vaddr, load_vaddr, len(blob), len(blob),
                    SEGMENT_ALIGN)
    out[phdrs_end:new_phdrs_end] = new_load.pack()

    return Result(bytes(out), load_vaddr, len(names), sce[DT_SCE_PLTRELSZ] // RELA_SIZE,
                  sce[DT_SCE_RELASZ] // RELA_SIZE)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("files", nargs="*", type=Path,
                        help="ELF files from self2elf (default: the pinned ELFs in analysis/elf/)")
    parser.add_argument("-o", "--out-dir", type=Path, default=DEFAULT_OUT,
                        help="output directory (default: analysis/elf-std/ in the main checkout)")
    args = parser.parse_args(argv)

    files = args.files
    if not files:
        if not DEFAULT_IN.is_dir():
            parser.error(f"no files given and {DEFAULT_IN} does not exist (run self2elf first)")
        # Only the pinned names, so stray files in analysis/elf/ don't fail the run.
        files = [DEFAULT_IN / name for name in sorted(set(KNOWN_FILES.values()))]

    # Checked before anything is written. analysis/elf/ holds the self2elf outputs this tool
    # verifies against, so writing there would break later runs.
    out_dir = args.out_dir.expanduser().resolve()
    forbidden = {p.resolve().parent for p in files} | {DEFAULT_IN.resolve()}
    game_dir = load_game_dir()
    if game_dir is not None:
        forbidden.add(game_dir)
    for parent in sorted(forbidden):
        if is_inside(out_dir, parent):
            print(f"error: refusing to write into the game, input or self2elf directory ({parent})",
                  file=sys.stderr)
            return 2

    failed = 0
    for path in files:
        try:
            known, data = read_pinned(path, KNOWN_FILES, PINNED_ELFS_HINT)
            result = convert(data)
        except (OSError, PinnedError, ElfError) as exc:
            print(f"{path}: error: {exc}", file=sys.stderr)
            failed += 1
            continue
        out_dir.mkdir(parents=True, exist_ok=True)
        out_path = out_dir / known
        out_path.write_bytes(result.data)
        print(f"{known}: symbols={result.symbols} plt_relocs={result.plt_relocs} "
              f"relocs={result.relocs} dynlibdata@{result.load_vaddr:#x} -> {out_path}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
