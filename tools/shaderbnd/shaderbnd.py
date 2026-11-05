#!/usr/bin/env python3
"""Extract the shader programs from the game's shader archives (dvdroot_ps4/shader/*.dcx).

Each archive is a DCX (zlib) wrapping a BND4 archive, whose entries hold one or more shader
programs. Only the 8 pinned archives are accepted. The formats are described in
docs/formats/dcx.md, bnd4.md and shader-binary.md (the program split, footer and metadata).

Output, per archive, under OUT/<archive without .dcx>/:
    entries/<entry path>       each BND4 entry as stored
    programs/<entry path>.<k>  program k of that entry
    index.json                 per program: entry name, program index, offset in the entry,
                               size, type and stage, code size and code hash (SHA-256 of the
                               code); per entry: the resource, sampler, input and output names

Usage: shaderbnd.py [-o DIR] [FILE ...]   (default: the 8 archives from BB_GAME_DIR)
       shaderbnd.py --scan-eboot [ELF]    (report only: the OrbShdr footers in eboot.elf)
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
import zlib
from collections import Counter
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "common"))
from cli import checked_out_dir, convert_each  # noqa: E402
from ps4elf import ELF_DIR, PINNED_ELFS, PINNED_ELFS_HINT, PT_LOAD, ElfError, parse  # noqa: E402
from shaderarchives import SHADER_ARCHIVES, SHADER_DIR  # noqa: E402
from toolenv import PinnedError, analysis_dir, load_game_dir, read_pinned  # noqa: E402


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


# Footer block: the marker, a version byte (7), then five u32. The first holds the program
# type in bits 2-5 and the code size in bits 8-31; bits 0-1 are 3 and bits 6-7 are 1 in every
# program of the archives. The third is the u32 at 0x04 of the entry header, the fourth is 0.
# The second and fifth are not interpreted.
FOOTER = struct.Struct("<7sB5I")
FOOTER_VERSION = 7
FOOTER_FLAGS = (0xC3, 0x43)  # (mask, value) of the bits around the program type
S_ENDPGM = bytes.fromhex("000081bf")

# Hardware stage of each program type. Types 2 and 3 are vertex shaders that run as ES and LS,
# 6 is the copy shader of a geometry shader and 8 a domain shader, both running as VS.
STAGES = ("ps", "vs", "es", "ls", "cs", "gs", "vs", "hs", "vs")


def read_footer(program: bytes, entry_hash: int) -> dict:
    """Type, stage, code size and code hash of one program as split_programs cuts it."""
    block = program.find(ORBSHDR)
    if block < 0 or block + FOOTER.size > len(program):
        raise FormatError("no complete OrbShdr footer block")
    _, version, info, _, block_hash, zero, _ = FOOTER.unpack_from(program, block)
    ptype, code_size = (info >> 2) & 0xF, info >> 8
    if version != FOOTER_VERSION or info & FOOTER_FLAGS[0] != FOOTER_FLAGS[1] \
            or ptype >= len(STAGES) or zero != 0:
        raise FormatError(f"unsupported footer block {program[block:block + FOOTER.size].hex()}")
    if block_hash != entry_hash:
        raise FormatError(f"footer value {block_hash:#x} differs from the entry header")
    if not len(S_ENDPGM) <= code_size <= block \
            or program[code_size - len(S_ENDPGM):code_size] != S_ENDPGM:
        raise FormatError(f"code size {code_size:#x} does not end after an s_endpgm")
    return {"type": ptype, "stage": STAGES[ptype], "code_size": code_size,
            "code_sha256": hashlib.sha256(program[:code_size]).hexdigest()}


# Metadata: at 0x24 plus the u32 at 0x10 of the entry header, after the last footer block and
# as many 16-byte data rows as the u32 at 0x38 says. A header of counts, record arrays in a
# fixed order, then a pool of NUL-terminated names that ends the entry. Names are referenced
# by u32 offsets relative to the field that holds them.
META_HEADER = struct.Struct("<4IBBH2I")  # resources, 0, constants, samplers, inputs, outputs,
#                                          stream-out records, 0, size of the name pool
RESOURCE = struct.Struct("<II4s3I")  # slot, size (constant buffers), kind bytes, 0, 0, name
CONSTANT_SIZE = 0x24  # skipped: the members of the constant buffers
SAMPLER = struct.Struct("<4I")  # slot, 0, not interpreted, name
VARYING = struct.Struct("<4sIII")  # kind bytes (byte 2: semantic index), 0, name, semantic
STREAM_OUT_SIZE = 0x10  # skipped
SHDR_META = 0x10
SHDR_DATA_ROWS = 0x38


def read_metadata(entry: bytes, after: int) -> dict:
    """The names of an entry's resources, samplers, inputs and outputs. `after` is the end of
    the last footer block."""
    start = SHDR_AT + struct.unpack_from("<I", entry, SHDR_META)[0]
    rows = struct.unpack_from("<I", entry, SHDR_DATA_ROWS)[0]
    if start != after + 16 * rows or start > len(entry) - META_HEADER.size:
        raise FormatError(f"metadata at {start:#x} does not follow the last footer block "
                          f"and {rows} data rows")
    (n_res, zero1, n_const, n_samp, n_in, n_out, n_so, zero2,
     pool_size) = META_HEADER.unpack_from(entry, start)
    pool = len(entry) - pool_size
    if zero1 or zero2 or not start + META_HEADER.size <= pool:
        raise FormatError("unsupported metadata header")

    def name(field: int) -> str:
        at = field + struct.unpack_from("<I", entry, field)[0]
        end = entry.find(b"\0", at) if pool <= at < len(entry) else -1
        if end < 0:
            raise FormatError(f"name offset at {field:#x} is outside the name pool")
        try:
            return entry[at:end].decode("ascii")
        except UnicodeDecodeError as exc:
            raise FormatError(f"name at {at:#x} is not ASCII") from exc

    pos = start + META_HEADER.size
    if pos + n_res * RESOURCE.size + n_const * CONSTANT_SIZE + n_samp * SAMPLER.size \
            + (n_in + n_out) * VARYING.size + n_so * STREAM_OUT_SIZE != pool:
        raise FormatError("metadata records do not end where the name pool starts")
    out: dict = {"resources": [], "samplers": [], "inputs": [], "outputs": []}
    for _ in range(n_res):
        slot, _, kind, *_ = RESOURCE.unpack_from(entry, pos)
        out["resources"].append({"name": name(pos + 0x14), "slot": slot, "kind": kind.hex()})
        pos += RESOURCE.size
    pos += n_const * CONSTANT_SIZE
    for _ in range(n_samp):
        slot = SAMPLER.unpack_from(entry, pos)[0]
        out["samplers"].append({"name": name(pos + 0xC), "slot": slot})
        pos += SAMPLER.size
    for i in range(n_in + n_out):
        kind = entry[pos:pos + 4]
        out["inputs" if i < n_in else "outputs"].append(
            {"name": name(pos + 8), "semantic": name(pos + 0xC), "index": kind[2],
             "kind": kind.hex()})
        pos += VARYING.size
    return out


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
    metadata: dict[str, dict]  # entry name: read_metadata


def extract(dcx: bytes) -> Archive:
    """Unpack an archive in memory, split its entries into programs and read their footers."""
    version, entries = unpack_bnd4(unpack_dcx(dcx))
    programs = []
    metadata = {}
    for entry in entries:
        path = entry_path(entry.name)
        try:
            spans = split_programs(entry.data)
            entry_hash = struct.unpack_from("<I", entry.data, 4)[0]
            rows = [{"entry": entry.name, "program": k, "offset": offset, "size": size,
                     "file": f"programs/{path}.{k}",
                     **read_footer(entry.data[offset:offset + size], entry_hash)}
                    for k, (offset, size) in enumerate(spans)]
            last_block = entry.data.find(ORBSHDR, spans[-1][0])
            metadata[entry.name] = read_metadata(entry.data, last_block + ORBSHDR_BLOCK)
        except FormatError as exc:
            raise FormatError(f"{entry.name}: {exc}") from exc
        programs += rows
    return Archive(version, entries, programs, metadata)


def scan_eboot(elf: bytes) -> Counter:
    """Report only: the OrbShdr footer blocks in a PS4 ELF, counted by loaded segment ("text",
    "data", "rodata" or "other"), stage ("invalid" if the block doesn't parse) and bits 0-1 of
    the type field."""
    loads = [ph for ph in parse(elf).phdrs if ph.p_type == PT_LOAD]
    counts: Counter = Counter()
    pos = elf.find(ORBSHDR)
    while pos >= 0:
        seg = next(("text" if ph.p_flags & 1 else "data" if ph.p_flags & 2 else "rodata"
                    for ph in loads if ph.p_offset <= pos < ph.p_offset + ph.p_filesz), "other")
        info = struct.unpack_from("<I", elf, pos + 8)[0] if pos + 12 <= len(elf) else 0
        ptype = (info >> 2) & 0xF
        valid = elf[pos + 7] == FOOTER_VERSION and info & 0xC0 == 0x40 and ptype < len(STAGES)
        counts[(seg, STAGES[ptype] if valid else "invalid", info & 3)] += 1
        pos = elf.find(ORBSHDR, pos + 1)
    return counts


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
             "programs": archive.programs, "metadata": archive.metadata}
    (base / "index.json").write_text(json.dumps(index, indent=1) + "\n", encoding="utf-8")
    return base


def default_inputs(game_dir: Path) -> list[Path]:
    return [game_dir / SHADER_DIR / name for name in sorted(SHADER_ARCHIVES.values())]


def report_eboot(path: Path) -> int:
    try:
        _, elf = read_pinned(path, {k: v for k, v in PINNED_ELFS.items() if v == "eboot.elf"},
                             PINNED_ELFS_HINT)
        counts = scan_eboot(elf)
    except (OSError, PinnedError, ElfError) as exc:
        print(f"{path.name}: error: {exc}", file=sys.stderr)
        return 1
    for (seg, stage, low_bits), n in sorted(counts.items()):
        print(f"{seg:6} {stage:7} bits 0-1 = {low_bits}: {n}")
    print(f"total: {sum(counts.values())} OrbShdr markers")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Extract the shader programs of the game.")
    parser.add_argument("files", nargs="*", type=Path,
                        help=f"shader archives (default: the 8 in BB_GAME_DIR/{SHADER_DIR})")
    parser.add_argument("-o", "--out-dir", type=Path, default=analysis_dir() / "shaders",
                        help="output directory (default: analysis/shaders/ in the main checkout)")
    parser.add_argument("--scan-eboot", type=Path, nargs="?", const=ELF_DIR / "eboot.elf",
                        metavar="ELF", help="only count the OrbShdr footers in the self2elf "
                        "output of eboot.bin (default: analysis/elf/eboot.elf)")
    args = parser.parse_args(argv)
    if args.scan_eboot:
        return report_eboot(args.scan_eboot)

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
