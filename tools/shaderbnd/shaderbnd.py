#!/usr/bin/env python3
"""Extract the shader programs from the game's shader archives (dvdroot_ps4/shader/*.dcx).

Each archive is a DCX (zlib) wrapping a BND4 archive, whose entries hold one or more shader
programs. Only the 8 pinned archives are accepted. The formats are described in
docs/formats/dcx.md, bnd4.md and shader-binary.md (the program split).

Output, per archive, under OUT/<archive without .dcx>/:
    entries/<entry path>       each BND4 entry as stored
    programs/<entry path>.<k>  program k of that entry
    index.json                 entry name, program index, offset in the entry and size

Usage: shaderbnd.py [-o DIR] [FILE ...]   (default: the 8 archives from BB_GAME_DIR)
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
import zlib
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "common"))
from cli import checked_out_dir, convert_each  # noqa: E402
from shaderarchives import SHADER_ARCHIVES, SHADER_DIR  # noqa: E402
from toolenv import analysis_dir, load_game_dir  # noqa: E402


class FormatError(Exception):
    pass


# DCX: big-endian. Every field before the zlib stream has the same value in all 8 archives;
# anything else is refused rather than guessed at.
DCX_HEADER = struct.Struct(">4sIIIII4sII4s4sI")  # up to and including the DCP size, 0x30 bytes
DCX_MAGIC = b"DCX\0"
DCX_VERSION = 0x10000
DCX_OFFSETS = (0x18, 0x24, 0x44, 0x4C)  # DCS, DCP, DCA, zlib stream
DCX_DCP_REST = bytes.fromhex("09000000" + "00" * 12 + "00010100")  # 0x30..0x43, unknown
DCX_DCA = struct.Struct(">4sI")  # at 0x44
DCX_DATA = 0x4C


def unpack_dcx(data: bytes) -> bytes:
    """The decompressed contents of a DCX file."""
    if len(data) < DCX_DATA:
        raise FormatError("truncated DCX header")
    (magic, version, *offsets, dcs, usize, csize, dcp, method, dcp_size) = \
        DCX_HEADER.unpack_from(data, 0)
    if magic != DCX_MAGIC:
        raise FormatError(f"not a DCX file (magic {magic!r})")
    if (version, tuple(offsets), dcs, dcp, dcp_size) != (DCX_VERSION, DCX_OFFSETS, b"DCS\0",
                                                          b"DCP\0", 0x20):
        raise FormatError("unsupported DCX header layout")
    if method != b"DFLT":
        raise FormatError(f"unsupported DCX method {method!r}")
    if data[DCX_HEADER.size:0x44] != DCX_DCP_REST or DCX_DCA.unpack_from(data, 0x44) != (b"DCA\0", 8):
        raise FormatError("unsupported DCX header layout")
    if DCX_DATA + csize != len(data):
        raise FormatError(f"compressed size {csize:#x} does not match the file "
                          f"({len(data) - DCX_DATA:#x} bytes after the header)")
    inflater = zlib.decompressobj()
    try:
        out = inflater.decompress(data[DCX_DATA:])
    except zlib.error as exc:
        raise FormatError(f"bad zlib stream: {exc}") from exc
    if not inflater.eof or inflater.unused_data:
        raise FormatError("zlib stream does not end at the end of the file")
    if len(out) != usize:
        raise FormatError(f"decompressed {len(out):#x} bytes, header says {usize:#x}")
    return out


# BND4: little-endian. Only the variant found in the shader archives is supported.
BND4_HEADER = struct.Struct("<4s8sIQ8sQQBB6sQ")  # 0x40 bytes
BND4_ENTRY = struct.Struct("<B3siQQIII")  # 0x24 bytes
BND4_MAGIC = b"BND4"
BND4_UNKNOWN = bytes.fromhex("0000000000000100")  # bytes 0x04..0x0B
BND4_FORMATS = (0x54, 0x74)  # byte 0x31; what the 0x20 bit means is unknown
ENTRY_FLAGS = 0x40


@dataclass
class Entry:
    name: str  # as stored, with backslashes
    data: bytes


def _utf16_name(data: bytes, offset: int, end: int) -> str:
    for pos in range(offset, end - 1, 2):
        if data[pos:pos + 2] == b"\0\0":
            return data[offset:pos].decode("utf-16-le")
    raise FormatError(f"name at {offset:#x} is not terminated before {end:#x}")


def unpack_bnd4(data: bytes) -> tuple[str, list[Entry]]:
    """The version string and the entries of a BND4 archive."""
    if len(data) < BND4_HEADER.size:
        raise FormatError("truncated BND4 header")
    (magic, unknown, count, header_size, version, entry_size, names_end, unicode, fmt,
     reserved, reserved2) = BND4_HEADER.unpack_from(data, 0)
    if magic != BND4_MAGIC:
        raise FormatError(f"not a BND4 archive (magic {magic!r})")
    if (unknown, header_size, entry_size, unicode, reserved, reserved2) != \
            (BND4_UNKNOWN, BND4_HEADER.size, BND4_ENTRY.size, 1, bytes(6), 0) \
            or fmt not in BND4_FORMATS:
        raise FormatError("unsupported BND4 variant")
    table_end = header_size + count * entry_size
    if not table_end <= names_end <= len(data):
        raise FormatError(f"name table end {names_end:#x} out of range")
    entries = []
    names = set()
    for i in range(count):
        flags, pad, minus_one, stored_size, size, offset, _id, name_offset = \
            BND4_ENTRY.unpack_from(data, header_size + i * entry_size)
        if (flags, pad, minus_one, stored_size) != (ENTRY_FLAGS, bytes(3), -1, size):
            raise FormatError(f"entry {i}: unsupported flags or compressed entry")
        if offset < names_end or offset + size > len(data):
            raise FormatError(f"entry {i}: data {offset:#x}+{size:#x} out of range")
        if not table_end <= name_offset < names_end:
            raise FormatError(f"entry {i}: name offset {name_offset:#x} out of range")
        name = _utf16_name(data, name_offset, names_end)
        if name in names:
            raise FormatError(f"entry {i}: duplicate name {name!r}")
        names.add(name)
        entries.append(Entry(name, data[offset:offset + size]))
    return version.rstrip(b"\0").decode("ascii"), entries


# Shader entries: a header with "Shdr" at 0x24, then the programs. Program k is its code
# followed by its footer: whatever lies before the 0x1C-byte block that starts with the
# OrbShdr marker, and that block. The next program's code follows the block directly. The
# last program also takes the data after its block, up to the end of the entry.
SHDR_TAG = b"Shdr"
SHDR_AT = 0x24
CODE_BASE = 0x34  # the u32 at 0x3C is the first program's code offset from here
ORBSHDR = b"OrbShdr"
ORBSHDR_BLOCK = 0x1C


def split_programs(entry: bytes) -> list[tuple[int, int]]:
    """(offset, size) of each program in a shader entry."""
    if entry[SHDR_AT:SHDR_AT + 4] != SHDR_TAG or len(entry) < CODE_BASE + 0xC:
        raise FormatError("no Shdr header at 0x24")
    start = CODE_BASE + struct.unpack_from("<I", entry, 0x3C)[0]
    starts = []
    marker = entry.find(ORBSHDR, start)
    while marker >= 0:
        if marker == start:
            raise FormatError(f"program {len(starts)} has no code before its footer")
        if marker + ORBSHDR_BLOCK > len(entry):
            raise FormatError(f"footer at {marker:#x} is truncated")
        starts.append(start)
        start = marker + ORBSHDR_BLOCK
        marker = entry.find(ORBSHDR, start)
    if not starts:
        raise FormatError("no OrbShdr footer")
    ends = starts[1:] + [len(entry)]
    return [(a, b - a) for a, b in zip(starts, ends)]


def entry_path(name: str) -> PurePosixPath:
    """The relative output path of an entry. Its backslashes become directories."""
    parts = name.split("\\")
    if any(p in ("", ".", "..") or "/" in p or "\0" in p for p in parts):
        raise FormatError(f"unsafe entry name {name!r}")
    return PurePosixPath(*parts)


@dataclass
class Archive:
    version: str
    entries: list[Entry]
    programs: list[dict]  # index.json rows


def extract(dcx: bytes) -> Archive:
    """Unpack an archive in memory and split its entries into programs."""
    version, entries = unpack_bnd4(unpack_dcx(dcx))
    programs = []
    for entry in entries:
        path = entry_path(entry.name)
        try:
            spans = split_programs(entry.data)
        except FormatError as exc:
            raise FormatError(f"{entry.name}: {exc}") from exc
        for k, (offset, size) in enumerate(spans):
            programs.append({"entry": entry.name, "program": k, "offset": offset, "size": size,
                             "file": f"programs/{path}.{k}"})
    return Archive(version, entries, programs)


def write_archive(out_dir: Path, name: str, archive: Archive) -> Path:
    """Writes the entries, programs and index.json. Returns the archive's directory."""
    base = out_dir / name.removesuffix(".dcx")
    data = {entry.name: entry.data for entry in archive.entries}
    for entry in archive.entries:
        target = base / "entries" / entry_path(entry.name)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(entry.data)
    for row in archive.programs:
        target = base / row["file"]
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data[row["entry"]][row["offset"]:row["offset"] + row["size"]])
    index = {"archive": name, "bnd4_version": archive.version, "entries": len(archive.entries),
             "programs": archive.programs}
    (base / "index.json").write_text(json.dumps(index, indent=1) + "\n", encoding="utf-8")
    return base


def default_inputs(game_dir: Path) -> list[Path]:
    return [game_dir / SHADER_DIR / name for name in sorted(SHADER_ARCHIVES.values())]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Extract the shader programs of the game.")
    parser.add_argument("files", nargs="*", type=Path,
                        help=f"shader archives (default: the 8 in BB_GAME_DIR/{SHADER_DIR})")
    parser.add_argument("-o", "--out-dir", type=Path, default=analysis_dir() / "shaders",
                        help="output directory (default: analysis/shaders/ in the main checkout)")
    args = parser.parse_args(argv)

    files = args.files
    if not files:
        game_dir = load_game_dir()
        if game_dir is None:
            parser.error("no files given and BB_GAME_DIR is not set (env or local.env)")
        files = default_inputs(game_dir)

    out_dir = checked_out_dir(args.out_dir, files)
    if out_dir is None:
        return 2

    def write(name: str, archive: Archive) -> None:
        base = write_archive(out_dir, name, archive)
        print(f"{name}: BND4 {archive.version}, {len(archive.entries)} entries, "
              f"{len(archive.programs)} programs -> {base}")

    return convert_each(files, SHADER_ARCHIVES, extract, write, (FormatError,),
                        hint="only the shader archives of CUSA03173 patch 1.09 are supported",
                        label=lambda path: path.name)


if __name__ == "__main__":
    sys.exit(main())
