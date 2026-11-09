#!/usr/bin/env python3
"""Rebuild the plain ELF embedded in a PS4 SELF (eboot.bin or a bundled .prx).

Only the unencrypted, uncompressed SELFs of the pinned game build are accepted.
The rebuilt ELF is checked against the SHA-256 digest stored in the SELF itself.
The format is described in docs/formats/self.md.

Usage:
    self2elf.py                    # eboot.bin and sce_module/*.prx from BB_GAME_DIR
    self2elf.py FILE [FILE ...]    # specific files
    self2elf.py -o DIR ...         # output directory (default: analysis/elf/)
"""

from __future__ import annotations

import argparse
import hashlib
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "common"))
from cli import checked_out_dir, convert_each  # noqa: E402
from toolenv import load_game_dir  # noqa: E402
from ps4elf import ELF_DIR, ELF_HEADER, ELF_PHDR, Phdr  # noqa: E402

# CUSA03173 patch 1.09. docs/source-policy.md lists the same hashes.
KNOWN_FILES = {
    "d65f0b4f01d59166aed16f8604196d8b7dd805abbf0758b356e8f1354c9429f9": "eboot.bin",
    "4378b47f46f1d856824a6f971db1a0c3f41833b77f49d2fba9538f667a139166": "sce_module/libc.prx",
    "89e134d3162323b29ad89c4af77b9db6501acca4ea382a3d62284c5e99584bd5": "sce_module/libSceFace.prx",
    "3b8eb4d5a5c5a7b381d8d0610077e6cd404c29695a686274802f6b2737f6bf22": "sce_module/libSceFios2.prx",
    "0ef7de31c741b20a50b641bdc576ff9df467509eb511e60cb43d7ee5f240b070": "sce_module/libSceHand.prx",
    "2ebfb4c208c3fbdd38c5f7a0f95f50b682ce2126a539c9be3fb9792ad761e1cb": "sce_module/libSceHeadTracker.prx",
    "86a854022a1ad1a2940787acd7e47c7a84e67148b2b641dba5452771506d20b9": "sce_module/libSceS3DConversion.prx",
    "43f433e5459033e201bea857002343bb2436c03c33401c4f0c938b3e197df1ab": "sce_module/libSceSmart.prx",
}

SELF_MAGIC = b"\x4f\x15\x3d\x1d"
SELF_HEADER = struct.Struct("<4s4sIHHIIHHI")  # 0x20 bytes
SELF_CONSTANT = b"\x00\x01\x01\x12"  # header bytes 4..7, same in every known SELF
SELF_ENTRY = struct.Struct("<QQQQ")  # 0x20 bytes
EXT_INFO = struct.Struct("<QQQQ32s")  # program identification header, 0x40 bytes

# Segment entry property bits
PROP_ENCRYPTED = 1 << 1
PROP_COMPRESSED = 1 << 3  # deflated
PROP_BLOCK_TABLE = 1 << 16  # entry holds a per-block digest table, not ELF data (our finding)

PT_SCE_LIBVERSION = 0x6FFFFF01


class SelfError(Exception):
    pass


@dataclass
class ElfImage:
    data: bytes
    e_type: int
    e_entry: int
    phdrs: list[Phdr]
    program_auth_id: int
    program_type: int


def _need(data: bytes, end: int, what: str) -> None:
    if end > len(data):
        raise SelfError(f"truncated file: {what} ends at {end:#x}, file is {len(data):#x}")


def self_to_elf(data: bytes) -> ElfImage:
    """Rebuild the embedded ELF. Raises SelfError on anything unexpected."""
    _need(data, SELF_HEADER.size, "SELF header")
    (magic, constant, _program_type, header_size, meta_size, file_size, _pad,
     num_entries, _unknown, _pad2) = SELF_HEADER.unpack_from(data, 0)
    if magic != SELF_MAGIC:
        raise SelfError(f"not a SELF (magic {magic.hex()})")
    if constant != SELF_CONSTANT:
        raise SelfError(f"unexpected SELF header bytes {constant.hex()}")

    entries_off = SELF_HEADER.size
    elf_off = entries_off + num_entries * SELF_ENTRY.size
    _need(data, elf_off + ELF_HEADER.size, "ELF header")
    entries = [SELF_ENTRY.unpack_from(data, entries_off + i * SELF_ENTRY.size)
               for i in range(num_entries)]

    (ident, e_type, _machine, _ver, e_entry, e_phoff, e_shoff, _eflags, e_ehsize,
     e_phentsize, e_phnum, _shentsize, e_shnum, _shstrndx) = ELF_HEADER.unpack_from(data, elf_off)
    if ident[:4] != b"\x7fELF" or ident[4] != 2 or ident[5] != 1:
        raise SelfError("embedded header is not a 64-bit little-endian ELF")
    if e_phoff != ELF_HEADER.size or e_phentsize != ELF_PHDR.size or e_ehsize != ELF_HEADER.size:
        raise SelfError("unexpected ELF header layout")
    if e_shoff != 0 or e_shnum != 0:
        raise SelfError("section headers are not supported")

    phdrs_end = elf_off + e_phoff + e_phnum * ELF_PHDR.size
    _need(data, phdrs_end, "program headers")
    phdrs = [Phdr(*ELF_PHDR.unpack_from(data, elf_off + e_phoff + i * ELF_PHDR.size))
             for i in range(e_phnum)]

    ext_off = (phdrs_end + 15) & ~15
    _need(data, ext_off + EXT_INFO.size, "extended info")
    program_auth_id, program_type, _app_ver, _fw_ver, elf_digest = EXT_INFO.unpack_from(data, ext_off)

    if ext_off + EXT_INFO.size > header_size or header_size + meta_size > len(data):
        raise SelfError("header/meta sizes do not fit the file")

    elf_size = max((p.p_offset + p.p_filesz for p in phdrs), default=phdrs_end - elf_off)
    out = bytearray(elf_size)
    out[: phdrs_end - elf_off] = data[elf_off:phdrs_end]
    covered: list[tuple[int, int]] = [(0, phdrs_end - elf_off)]

    for i, (props, offset, filesz, memsz) in enumerate(entries):
        if props & PROP_BLOCK_TABLE:
            continue
        if props & (PROP_ENCRYPTED | PROP_COMPRESSED):
            raise SelfError(f"segment entry {i} is encrypted or compressed ({props:#x})")
        index = props >> 20
        if index >= len(phdrs):
            raise SelfError(f"segment entry {i} points at program header {index}")
        ph = phdrs[index]
        if filesz != ph.p_filesz or memsz != ph.p_filesz:
            raise SelfError(f"segment entry {i} size {filesz:#x} != p_filesz {ph.p_filesz:#x}")
        _need(data, offset + filesz, f"segment entry {i}")
        out[ph.p_offset: ph.p_offset + filesz] = data[offset: offset + filesz]
        covered.append((ph.p_offset, ph.p_offset + filesz))

    # The SCE_LIBVERSION segment has no entry; its bytes follow the signed part of the file.
    trailer = data[file_size:]
    if trailer:
        libver = [p for p in phdrs if p.p_type == PT_SCE_LIBVERSION]
        if len(libver) != 1 or libver[0].p_filesz != len(trailer):
            raise SelfError(f"{len(trailer):#x} trailing bytes do not match an SCE_LIBVERSION segment")
        ph = libver[0]
        out[ph.p_offset: ph.p_offset + ph.p_filesz] = trailer
        covered.append((ph.p_offset, ph.p_offset + ph.p_filesz))

    for i, ph in enumerate(phdrs):
        if ph.p_filesz and not any(a <= ph.p_offset and ph.p_offset + ph.p_filesz <= b
                                   for a, b in covered):
            raise SelfError(f"no data for program header {i} (type {ph.p_type:#x})")

    if hashlib.sha256(out).digest() != elf_digest:
        raise SelfError("rebuilt ELF does not match the digest stored in the SELF")

    return ElfImage(bytes(out), e_type, e_entry, phdrs, program_auth_id, program_type)


def default_inputs(game_dir: Path) -> list[Path]:
    eboot = game_dir / "eboot.bin"
    prx = sorted(p for p in (game_dir / "sce_module").glob("*.prx") if not p.name.startswith("._"))
    return [eboot, *prx]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Rebuild the ELF embedded in a PS4 SELF.")
    parser.add_argument("files", nargs="*", type=Path,
                        help="SELF files (default: eboot.bin and sce_module/*.prx from BB_GAME_DIR)")
    parser.add_argument("-o", "--out-dir", type=Path, default=ELF_DIR,
                        help="output directory (default: analysis/elf/ in the main checkout)")
    args = parser.parse_args(argv)

    game_dir = load_game_dir()
    files = args.files
    if not files:
        if game_dir is None:
            parser.error("no files given and BB_GAME_DIR is not set (env or local.env)")
        files = default_inputs(game_dir)

    out_dir = checked_out_dir(args.out_dir, files)
    if out_dir is None:
        return 2

    def write(known: str, image: ElfImage) -> None:
        out_dir.mkdir(parents=True, exist_ok=True)
        out_path = out_dir / (Path(known).stem + ".elf")
        out_path.write_bytes(image.data)
        print(f"{known}: e_type={image.e_type:#06x} entry={image.e_entry:#x} "
              f"phdrs={len(image.phdrs)} size={len(image.data):#x} digest ok -> {out_path}")

    return convert_each(files, KNOWN_FILES, self_to_elf, write, (SelfError,),
                        label=lambda path: path.name)


if __name__ == "__main__":
    sys.exit(main())
