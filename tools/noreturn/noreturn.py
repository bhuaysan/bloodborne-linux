#!/usr/bin/env python3
"""Find the imports of a PS4 ELF that never return, for Ghidra's analysis.

Ghidra does not know that an import such as the stack protector's failure handler never returns.
It decodes the bytes after every call to it, often a jump table or the next function, and
reports bad instructions there. This tool finds those imports from the ELF itself:

A call to a function that returns cannot be the last instruction of a function, because the
caller still has to return or jump somewhere. So when a direct call through a PLT stub is the
last instruction of an FDE's range (followed at most by nop or int3 padding), the import is
probably non-returning. An import counts as non-returning when at least MIN_ENDING of its call
sites end a function and they are at least half of its call sites. A single match is not
enough: a function that itself never returns may end with a call to an ordinary import.

The format of the unwind tables is described in docs/formats/eh-frame.md.

Output, under analysis/noreturn/ (untracked, never commit it):
    <file>.txt    one line per PLT stub of a non-returning import: "<vaddr hex> <symbol>",
                  the input of ghidra_scripts/MarkNoReturn.java

Usage:
    noreturn.py                    # analysis/elf/eboot.elf
    noreturn.py FILE [FILE ...]    # specific self2elf outputs
    noreturn.py -o DIR ...         # output directory (default: analysis/noreturn/)
"""

from __future__ import annotations

import argparse
import re
import struct
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "common"))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "elf2std"))
from cli import checked_out_dir, convert_each  # noqa: E402
from toolenv import analysis_dir  # noqa: E402
from ps4elf import (  # noqa: E402
    DT_SCE_JMPREL, DT_SCE_PLTRELSZ, ELF_DIR, PINNED_ELFS, PINNED_ELFS_HINT, PT_LOAD, RELA,
    ElfError, need, parse,
)
from elf2std import EH_PE_FORMATS, PF_X, PT_GNU_EH_FRAME, file_offset  # noqa: E402

DEFAULT_OUT = analysis_dir() / "noreturn"

R_X86_64_JUMP_SLOT = 7
MIN_ENDING = 2

# Encodings of .eh_frame_hdr this tool reads: version 1, eh_frame_ptr pcrel|sdata4, fde_count
# udata4, search table datarel|sdata4 (the values in every pinned file).
EH_HDR_ENCODINGS = bytes([1, 0x1B, 0x03, 0x3B])
EH_PE_PCREL = 0x10

# Padding that may follow the last call of a function: the recommended nop encodings and int3.
PADDING = (b"\x66\x0f\x1f\x84\x00\x00\x00\x00\x00", b"\x0f\x1f\x84\x00\x00\x00\x00\x00",
           b"\x0f\x1f\x80\x00\x00\x00\x00", b"\x66\x0f\x1f\x44\x00\x00",
           b"\x0f\x1f\x44\x00\x00", b"\x0f\x1f\x40\x00", b"\x0f\x1f\x00", b"\x66\x90", b"\x90",
           b"\xcc")
MAX_PADDING = 15


@dataclass
class Import:
    symbol: str  # NID#library#module
    stubs: list[int]
    calls: int = 0  # direct calls to one of the stubs
    ending: int = 0  # of those, calls that end a function

    @property
    def noreturn(self) -> bool:
        return self.ending >= MIN_ENDING and 2 * self.ending >= self.calls


def uleb(data: bytes, pos: int) -> tuple[int, int]:
    value = shift = 0
    while True:
        need(pos < len(data), "truncated LEB128 value")
        byte = data[pos]
        value |= (byte & 0x7F) << shift
        pos += 1
        shift += 7
        if byte < 0x80:
            return value, pos


def fde_encoding(data: bytes, cie: int) -> int:
    """Pointer encoding of the FDEs of the CIE at file offset cie (augmentation 'R')."""
    (length,) = struct.unpack_from("<I", data, cie)
    need(length not in (0, 0xFFFFFFFF), f"unsupported CIE at offset {cie:#x}")
    end = cie + 4 + length
    need(struct.unpack_from("<I", data, cie + 4)[0] == 0, f"no CIE at offset {cie:#x}")
    version = data[cie + 8]
    aug_end = data.index(b"\0", cie + 9, end)
    aug = data[cie + 9: aug_end].decode("ascii")
    pos = aug_end + 1
    _, pos = uleb(data, pos)  # code alignment
    _, pos = uleb(data, pos)  # data alignment (signed, only skipped)
    pos = pos + 1 if version == 1 else uleb(data, pos)[1]  # return address register
    encoding = 0  # absptr
    if aug.startswith("z"):
        _, pos = uleb(data, pos)
        for char in aug[1:]:
            if char == "R":
                encoding = data[pos]
                pos += 1
            elif char == "L":
                pos += 1
            elif char == "P":
                fmt = EH_PE_FORMATS.get(data[pos] & 0x0F)
                need(fmt is not None, f"unsupported personality encoding at offset {cie:#x}")
                pos += 1 + struct.calcsize(fmt)
            else:
                raise ElfError(f"unsupported CIE augmentation {aug!r} at offset {cie:#x}")
    need(pos <= end, f"CIE at offset {cie:#x} overruns its length")
    return encoding


def fde_ranges(data: bytes, phdrs, loads) -> list[tuple[int, int]]:
    """(start, end) of every FDE in the .eh_frame_hdr search table."""
    hdrs = [ph for ph in phdrs if ph.p_type == PT_GNU_EH_FRAME]
    need(len(hdrs) == 1, "expected one PT_GNU_EH_FRAME")
    hdr_vaddr = hdrs[0].p_vaddr
    hdr, _ = file_offset(data, loads, hdr_vaddr, 12)
    need(data[hdr: hdr + 4] == EH_HDR_ENCODINGS,
         f"unsupported .eh_frame_hdr encodings {data[hdr: hdr + 4].hex()}")
    (count,) = struct.unpack_from("<I", data, hdr + 8)
    file_offset(data, loads, hdr_vaddr, 12 + 8 * count)
    encodings: dict[int, int] = {}
    out = []
    for i in range(count):
        _, fde_rel = struct.unpack_from("<ii", data, hdr + 12 + 8 * i)
        fde_vaddr = hdr_vaddr + fde_rel
        fde, _ = file_offset(data, loads, fde_vaddr, 8)
        cie_ptr = struct.unpack_from("<I", data, fde + 4)[0]
        need(struct.unpack_from("<I", data, fde)[0] not in (0, 0xFFFFFFFF) and cie_ptr != 0,
             f"no FDE at {fde_vaddr:#x}")
        cie_vaddr = fde_vaddr + 4 - cie_ptr
        if cie_vaddr not in encodings:
            encodings[cie_vaddr] = fde_encoding(data, file_offset(data, loads, cie_vaddr, 9)[0])
        enc = encodings[cie_vaddr]
        fmt = EH_PE_FORMATS.get(enc & 0x0F)
        need(fmt is not None and enc & 0xF0 in (0, EH_PE_PCREL),
             f"unsupported FDE pointer encoding {enc:#x} at {fde_vaddr:#x}")
        size = struct.calcsize(fmt)
        file_offset(data, loads, fde_vaddr + 8, 2 * size)
        begin, length = struct.unpack_from("<" + fmt[1] * 2, data, fde + 8)
        if enc & 0xF0 == EH_PE_PCREL:
            begin += fde_vaddr + 8
        begin &= 0xFFFFFFFFFFFFFFFF
        if length:
            out.append((begin, begin + length))
    return out


def is_padding(chunk: bytes) -> bool:
    while chunk:
        for nop in PADDING:
            if chunk.startswith(nop):
                chunk = chunk[len(nop):]
                break
        else:
            return False
    return True


def analyze(data: bytes) -> list[Import]:
    """Every import that has a PLT stub, with its call counts, sorted by symbol."""
    image = parse(data)
    loads = [ph for ph in image.phdrs if ph.p_type == PT_LOAD]
    text = [ph for ph in loads if ph.p_flags & PF_X]
    need(len(text) == 1, "expected one executable PT_LOAD")
    (text,) = text

    jmprel, size = image.tables[DT_SCE_JMPREL], image.tables[DT_SCE_PLTRELSZ]
    slots: dict[int, str] = {}
    for off in range(jmprel, jmprel + size, RELA.size):
        r_offset, r_info, _ = RELA.unpack_from(image.blob, off)
        if r_info & 0xFFFFFFFF == R_X86_64_JUMP_SLOT:
            index = r_info >> 32
            need(index < len(image.symbols), f"bad symbol index in relocation at {r_offset:#x}")
            slots[r_offset] = image.symbols[index].name.decode()

    code = data[text.p_offset: text.p_offset + text.p_filesz]
    base = text.p_vaddr
    imports: dict[str, Import] = {}
    stub_of: dict[int, Import] = {}
    for match in re.finditer(rb"\xff\x25", code):  # jmp [rip + disp32]
        pos = match.start()
        if pos + 6 > len(code):
            continue
        (disp,) = struct.unpack_from("<i", code, pos + 2)
        symbol = slots.get(base + pos + 6 + disp)
        if symbol:
            imp = imports.setdefault(symbol, Import(symbol, []))
            imp.stubs.append(base + pos)
            stub_of[base + pos] = imp

    def call_target(pos: int) -> Import | None:  # call rel32 at code[pos]
        if pos < 0 or pos + 5 > len(code) or code[pos] != 0xE8:
            return None
        (disp,) = struct.unpack_from("<i", code, pos + 1)
        return stub_of.get(base + pos + 5 + disp)

    for match in re.finditer(rb"\xe8", code):
        imp = call_target(match.start())
        if imp:
            imp.calls += 1
    for start, end in fde_ranges(data, image.phdrs, loads):
        if not (base <= start < end <= base + len(code)):
            continue
        end_pos = end - base
        for pad in range(min(MAX_PADDING, end - start - 5) + 1):
            if is_padding(code[end_pos - pad: end_pos]):
                imp = call_target(end_pos - pad - 5)
                if imp:
                    imp.ending += 1
                    break
    return sorted(imports.values(), key=lambda imp: imp.symbol)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("files", nargs="*", type=Path,
                        help="ELF files from self2elf (default: analysis/elf/eboot.elf)")
    parser.add_argument("-o", "--out-dir", type=Path, default=DEFAULT_OUT,
                        help="output directory (default: analysis/noreturn/ in the main checkout)")
    args = parser.parse_args(argv)
    files = args.files or [ELF_DIR / "eboot.elf"]

    out_dir = checked_out_dir(args.out_dir, files, ELF_DIR)
    if out_dir is None:
        return 2

    def write(known: str, imports: list[Import]) -> None:
        found = [imp for imp in imports if imp.noreturn]
        print(f"{known}: {len(found)} non-returning of {len(imports)} imports with a PLT stub")
        for imp in sorted(imports, key=lambda imp: -imp.ending):
            if imp.ending:
                mark = "noreturn" if imp.noreturn else "-"
                print(f"  {imp.symbol:24} calls={imp.calls:6} ending={imp.ending:6} {mark}")
        out_dir.mkdir(parents=True, exist_ok=True)
        out_path = out_dir / (Path(known).stem + ".txt")
        out_path.write_text("".join(f"{stub:x} {imp.symbol}\n"
                                    for imp in found for stub in imp.stubs))
        print(f"  -> {out_path}")

    return convert_each(files, PINNED_ELFS, analyze, write, (ElfError,), PINNED_ELFS_HINT)


if __name__ == "__main__":
    sys.exit(main())
