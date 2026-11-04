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
- repoints PT_DYNAMIC at the new dynamic table,
- adds section headers for .eh_frame_hdr and .eh_frame (found through PT_GNU_EH_FRAME), so
  Ghidra finds the unwind tables and creates a function for every FDE. With them come the null
  section, .shstrtab, .dynamic and .dynstr (for readelf) and sections covering the rest of the
  original segments (for Ghidra's memory blocks, see fill_segments).

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
    DT_SYMTAB, DYN, ELF_DIR, EM_X86_64, ET_EXEC, ET_SCE_DYNAMIC, ET_SCE_DYNEXEC, PINNED_ELFS,
    PINNED_ELFS_HINT, PT_DYNAMIC, PT_LOAD, PT_SCE_DYNLIBDATA, RELA_SIZE, SYM, ElfError, need, parse,
)

DEFAULT_OUT = analysis_dir() / "elf-std"

PF_X, PF_W, PF_R = 1, 2, 4
SEGMENT_ALIGN = 0x4000
PT_GNU_EH_FRAME = 0x6474E550

ELF_SHDR = struct.Struct("<IIQQQQIIQQ")  # 0x40 bytes
SHT_PROGBITS = 1
SHT_STRTAB = 3
SHT_DYNAMIC = 6
SHT_NOBITS = 8
SHF_WRITE, SHF_ALLOC, SHF_EXECINSTR = 1, 2, 4

# DWARF pointer encodings (DW_EH_PE_*) accepted for eh_frame_ptr in .eh_frame_hdr.
EH_PE_FORMATS = {0x00: "<Q", 0x03: "<I", 0x04: "<Q", 0x0B: "<i", 0x0C: "<q"}
EH_PE_ABSPTR, EH_PE_PCREL, EH_PE_DATAREL = 0x00, 0x10, 0x30

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
    eh_frame: tuple[int, int] | None = None  # (vaddr, size) of .eh_frame, if found
    fdes: int = 0  # FDEs in .eh_frame, including ones left out of the .eh_frame_hdr table


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


def file_offset(data: bytes, loads: list[Phdr], vaddr: int, size: int = 1) -> tuple[int, Phdr]:
    """File offset of vaddr..vaddr+size, which must lie in the file data of one PT_LOAD."""
    for ph in loads:
        if ph.p_vaddr <= vaddr and vaddr + size <= ph.p_vaddr + ph.p_filesz:
            offset = ph.p_offset + vaddr - ph.p_vaddr
            need(offset + size <= len(data), f"{vaddr:#x} lies past the end of the file")
            return offset, ph
    raise ElfError(f"{vaddr:#x}+{size:#x} is not in the file data of a PT_LOAD segment")


def walk_eh_frame(data: bytes, loads: list[Phdr], start: int, limit: int) -> tuple[int, int]:
    """Walks the CIE/FDE records of .eh_frame from start (vaddr).

    Stops after the zero terminator, or at limit (the end of the containing segment, or the start
    of .eh_frame_hdr when it follows). Returns (size of .eh_frame, number of FDEs)."""
    base, _ = file_offset(data, loads, start)
    pos, fdes = 0, 0
    while start + pos < limit:
        need(start + pos + 4 <= limit, f".eh_frame record at {start + pos:#x} crosses {limit:#x}")
        (length,) = struct.unpack_from("<I", data, base + pos)
        if length == 0:
            return pos + 4, fdes  # the terminator belongs to the section
        header = 4
        if length == 0xFFFFFFFF:  # 64-bit DWARF: the real length follows
            need(start + pos + 12 <= limit,
                 f".eh_frame record at {start + pos:#x} crosses {limit:#x}")
            (length,) = struct.unpack_from("<Q", data, base + pos + 4)
            header = 12
        end = pos + header + length
        need(length >= 4 and start + end <= limit,
             f".eh_frame record at {start + pos:#x} has a bad length {length:#x}")
        (cie_id,) = struct.unpack_from("<I", data, base + pos + header)
        fdes += cie_id != 0  # CIEs have id 0 in .eh_frame, FDEs a CIE pointer
        pos = end
    return pos, fdes


def find_eh_frame(data: bytes, phdrs: list[Phdr],
                  loads: list[Phdr]) -> tuple[Phdr, int, int, int] | None:
    """Locates .eh_frame through PT_GNU_EH_FRAME (.eh_frame_hdr).

    Returns (the PT_GNU_EH_FRAME header, .eh_frame vaddr, size, FDE count), or None."""
    hdrs = [ph for ph in phdrs if ph.p_type == PT_GNU_EH_FRAME]
    if not hdrs:
        return None
    need(len(hdrs) == 1, "more than one PT_GNU_EH_FRAME")
    (eh_hdr,) = hdrs
    need(eh_hdr.p_filesz >= 4, "PT_GNU_EH_FRAME is too small")
    hdr_off, hdr_load = file_offset(data, loads, eh_hdr.p_vaddr, eh_hdr.p_filesz)
    version, ptr_enc = data[hdr_off], data[hdr_off + 1]
    need(version == 1, f"unsupported .eh_frame_hdr version {version}")
    fmt = EH_PE_FORMATS.get(ptr_enc & 0x0F)
    app = ptr_enc & 0xF0
    need(fmt is not None and app in (EH_PE_ABSPTR, EH_PE_PCREL, EH_PE_DATAREL),
         f"unsupported eh_frame_ptr encoding {ptr_enc:#x}")
    need(4 + struct.calcsize(fmt) <= eh_hdr.p_filesz, "PT_GNU_EH_FRAME is too small")
    (value,) = struct.unpack_from(fmt, data, hdr_off + 4)
    # pcrel: relative to the field itself, datarel: relative to the start of .eh_frame_hdr.
    base = {EH_PE_ABSPTR: 0, EH_PE_PCREL: eh_hdr.p_vaddr + 4, EH_PE_DATAREL: eh_hdr.p_vaddr}[app]
    start = (base + value) & 0xFFFFFFFFFFFFFFFF
    _, load = file_offset(data, loads, start)
    limit = load.p_vaddr + load.p_filesz
    if load is hdr_load and start < eh_hdr.p_vaddr:
        limit = min(limit, eh_hdr.p_vaddr)
    size, fdes = walk_eh_frame(data, loads, start, limit)
    return eh_hdr, start, size, fdes


@dataclass
class Section:
    name: bytes
    sh_type: int
    vaddr: int
    offset: int
    size: int
    align: int
    entsize: int = 0
    link: bytes = b""  # name of the linked section
    flags: int = SHF_ALLOC


def fill_segments(loads: list[Phdr], sections: list[Section]) -> list[Section]:
    """Sections for the parts of each PT_LOAD that the given sections don't cover.

    With section headers present, Ghidra builds its memory blocks from the sections and loads
    only the file data of uncovered segment parts, without their execute permission or the
    zero-filled tail. So every loaded byte gets a section: .text (executable), .data (writable),
    .rodata (other) and .bss for the part past p_filesz."""
    fillers = []
    for ph in loads:
        flags = (SHF_ALLOC | (SHF_EXECINSTR if ph.p_flags & PF_X else 0)
                 | (SHF_WRITE if ph.p_flags & PF_W else 0))
        name = b".text" if ph.p_flags & PF_X else b".data" if ph.p_flags & PF_W else b".rodata"
        end = ph.p_vaddr + ph.p_filesz
        pos = ph.p_vaddr
        inside = sorted((sec for sec in sections if ph.p_vaddr <= sec.vaddr < end),
                        key=lambda sec: sec.vaddr)
        for start, stop in [(sec.vaddr, sec.vaddr + sec.size) for sec in inside] + [(end, end)]:
            if start > pos:
                fillers.append(Section(name, SHT_PROGBITS, pos, ph.p_offset + pos - ph.p_vaddr,
                                       start - pos, 1, flags=flags))
            pos = max(pos, stop)
        if ph.p_memsz > ph.p_filesz:
            fillers.append(Section(b".bss", SHT_NOBITS, end, ph.p_offset + ph.p_filesz,
                                   ph.p_memsz - ph.p_filesz, 1, flags=flags))
    return fillers


def section_headers(sections: list[Section], strtab_off: int) -> tuple[bytes, bytes]:
    """Returns (.shstrtab contents, section header table) for loaded sections.

    The table gets the null section first, then the sections sorted by address, then .shstrtab."""
    sections = sorted(sections, key=lambda sec: sec.vaddr)
    for prev, sec in zip(sections, sections[1:]):
        need(prev.vaddr + prev.size <= sec.vaddr,
             f"sections {prev.name.decode()} and {sec.name.decode()} overlap")
    index = {sec.name: i for i, sec in enumerate(sections, start=1)}
    names = bytearray(b"\0")
    table = bytearray(ELF_SHDR.size)  # index 0: the null section
    for sec in sections:
        table += ELF_SHDR.pack(len(names), sec.sh_type, sec.flags, sec.vaddr, sec.offset, sec.size,
                               index[sec.link] if sec.link else 0, 0, sec.align, sec.entsize)
        names += sec.name + b"\0"
    name = len(names)
    names += b".shstrtab\0"
    table += ELF_SHDR.pack(name, SHT_STRTAB, 0, 0, strtab_off, len(names), 0, 0, 1, 0)
    return bytes(names), bytes(table)


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

    new_load = Phdr(PT_LOAD, PF_R, load_offset, load_vaddr, load_vaddr, len(blob), len(blob),
                    SEGMENT_ALIGN)
    out_loads = loads + [new_load]

    # Section headers for the unwind tables, after everything else. They are not loaded.
    eh_frame = find_eh_frame(data, phdrs, loads)
    eh_range, fdes = None, 0
    if eh_frame is not None:
        eh_hdr, eh_start, eh_size, fdes = eh_frame
        eh_range = (eh_start, eh_size)

        def section(name: bytes, sh_type: int, vaddr: int, size: int, align: int,
                    **kwargs) -> Section:
            offset, _ = file_offset(out, out_loads, vaddr, size)
            return Section(name, sh_type, vaddr, offset, size, align, **kwargs)

        # .dynamic and .dynstr are needed too: with section headers present, readelf looks up
        # the dynamic table through its section, and the string table through its sh_link.
        sections = [
            section(b".eh_frame", SHT_PROGBITS, eh_start, eh_size, 4),
            section(b".eh_frame_hdr", SHT_PROGBITS, eh_hdr.p_vaddr, eh_hdr.p_filesz, 4),
            section(b".dynstr", SHT_STRTAB, load_vaddr + sce[DT_SCE_STRTAB], sce[DT_SCE_STRSZ], 1),
            section(b".dynamic", SHT_DYNAMIC, load_vaddr + dyn_off, dyn_size, 8,
                    entsize=DYN.size, link=b".dynstr"),
        ]
        # Not for the new segment: it is read-only and fully backed by the file, so Ghidra loads
        # its uncovered parts correctly, and a section over the symbol and relocation tables
        # there makes Ghidra stop turning the PLT stubs into thunks of the imports.
        sections += fill_segments(loads, sections)
        shstrtab, table = section_headers(sections, len(out))
        out += shstrtab
        out += bytes(align_up(len(out), 8) - len(out))
        e_shoff, e_shentsize = len(out), ELF_SHDR.size
        e_shnum, e_shstrndx = len(table) // ELF_SHDR.size, len(table) // ELF_SHDR.size - 1
        out += table

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
    out[phdrs_end:new_phdrs_end] = new_load.pack()

    return Result(bytes(out), load_vaddr, len(names), sce[DT_SCE_PLTRELSZ] // RELA_SIZE,
                  sce[DT_SCE_RELASZ] // RELA_SIZE, eh_range, fdes)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("files", nargs="*", type=Path,
                        help="ELF files from self2elf (default: the pinned ELFs in analysis/elf/)")
    parser.add_argument("-o", "--out-dir", type=Path, default=DEFAULT_OUT,
                        help="output directory (default: analysis/elf-std/ in the main checkout)")
    args = parser.parse_args(argv)

    files = args.files
    if not files:
        if not ELF_DIR.is_dir():
            parser.error(f"no files given and {ELF_DIR} does not exist (run self2elf first)")
        # Only the pinned names, so stray files in analysis/elf/ don't fail the run.
        files = [ELF_DIR / name for name in sorted(set(PINNED_ELFS.values()))]

    # Checked before anything is written. analysis/elf/ holds the self2elf outputs this tool
    # verifies against, so writing there would break later runs.
    out_dir = args.out_dir.expanduser().resolve()
    forbidden = {p.resolve().parent for p in files} | {ELF_DIR.resolve()}
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
            known, data = read_pinned(path, PINNED_ELFS, PINNED_ELFS_HINT)
            result = convert(data)
        except (OSError, PinnedError, ElfError) as exc:
            print(f"{path}: error: {exc}", file=sys.stderr)
            failed += 1
            continue
        out_dir.mkdir(parents=True, exist_ok=True)
        out_path = out_dir / known
        out_path.write_bytes(result.data)
        eh = (f" eh_frame@{result.eh_frame[0]:#x}+{result.eh_frame[1]:#x} fdes={result.fdes}"
              if result.eh_frame else "")
        print(f"{known}: symbols={result.symbols} plt_relocs={result.plt_relocs} "
              f"relocs={result.relocs} dynlibdata@{result.load_vaddr:#x}{eh} -> {out_path}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
