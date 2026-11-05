"""Tests for shaderbnd. Run: python3 -m unittest discover -s tools/shaderbnd

The synthetic tests need no game files. The game tests run only when BB_GAME_DIR is set
(environment or local.env).
"""

import hashlib
import json
import struct
import tempfile
import unittest
import zlib
from collections import Counter
from pathlib import Path

import shaderbnd as sb
from elfbuilder import Builder
from toolenv import read_pinned

S_ENDPGM = bytes.fromhex("000081bf")


ENTRY_HASH = 0xA74BEE45


def footer_block(code_size: int, ptype: int) -> bytes:
    return sb.FOOTER.pack(b"OrbShdr", 7, code_size << 8 | ptype << 2 | 0x43, 0x0C0302,
                          ENTRY_HASH, 0, 0x92D814C0)


def make_metadata() -> bytes:
    """Metadata with two resources (a constant buffer and a texture), one constant buffer
    member, one sampler, one input, one output and one stream-out record. A str field is a
    name, stored as its offset from the field."""
    records = [[("I", 3), ("I", 0x50), ("4s", bytes.fromhex("16046a00")), ("I", 1), ("I", 0),
                ("I", "cbParam")],
               [("I", 1), ("I", 0), ("4s", bytes.fromhex("02050300")), ("I", 0), ("I", 0),
                ("I", "g_Texture0")],
               [("36s", bytes(range(36)))],
               [("I", 2), ("I", 0), ("I", 7), ("I", "SS_ClampLinear")],
               [("4s", bytes.fromhex("03240100")), ("I", 0), ("I", "IN.tex0"), ("I", "TEXCOORD")],
               [("4s", bytes.fromhex("03380000")), ("I", 0), ("I", "Main"), ("I", "(no_name)")],
               [("16s", bytes(16))]]
    names = [v for rec in records for _, v in rec if isinstance(v, str)]
    pool = b"".join(n.encode() + b"\0" for n in names)
    body = sb.META_HEADER.size + sum(struct.calcsize("<" + c) for rec in records for c, _ in rec)
    at = {n: body + pool.index(n.encode() + b"\0") for n in names}
    out = bytearray(sb.META_HEADER.pack(2, 0, 1, 1, 1, 1, 1, 0, len(pool)))
    for rec in records:
        for code, value in rec:
            field = at[value] - len(out) if isinstance(value, str) else value
            out += struct.pack("<" + code, field)
    return bytes(out + pool)


def make_shader_entry(codes: list[bytes], pre: bytes = b"\x6a\x02\x00\x08",
                      types: list[int] | None = None,
                      gap: bytes = b"") -> tuple[bytes, list[tuple[int, int]]]:
    """A shader entry with one program per code blob, and the expected (offset, size) list.
    `gap` goes between the last footer block and the metadata."""
    header = bytearray(0x50)
    for at, value in sb.SHDR_FIXED:
        header[at:at + len(value)] = value
    header[0x04:0x08] = ENTRY_HASH.to_bytes(4, "little")
    header[0x24:0x28] = b"Shdr"
    struct.pack_into("<I", header, 0x3C, len(header) - 0x34)
    out = bytearray(header)
    spans = []
    for code, ptype in zip(codes, types or [0] * len(codes)):
        start = len(out)
        out += code + S_ENDPGM + pre + footer_block(len(code) + 4, ptype)
        spans.append(start)
    out += gap
    struct.pack_into("<I", out, 0x38, len(gap) // 16)
    struct.pack_into("<I", out, 0x10, len(out) - 0x24)
    out += make_metadata()
    ends = spans[1:] + [len(out)]
    return bytes(out), [(a, b - a) for a, b in zip(spans, ends)]


def make_bnd4(files: list[tuple[str, bytes]], fmt: int = 0x54) -> bytes:
    """A BND4 archive laid out like the game's: header, entry table, UTF-16 names, data."""
    table_end = 0x40 + 0x24 * len(files)
    names = b""
    name_offsets = []
    for name, _ in files:
        name_offsets.append(table_end + len(names))
        names += name.encode("utf-16-le") + b"\0\0"
    names_end = table_end + len(names)
    data = b""
    data_offsets = []
    for _, blob in files:
        pos = (names_end + len(data) + 15) & ~15
        data += bytes(pos - names_end - len(data)) + blob
        data_offsets.append(pos)
    header = sb.BND4_HEADER.pack(b"BND4", sb.BND4_UNKNOWN, len(files), 0x40, b"10G29R1\0", 0x24,
                                 names_end, 1, fmt, bytes(6), 0)
    table = b"".join(sb.BND4_ENTRY.pack(0x40, bytes(3), -1, len(blob), len(blob), off, i, noff)
                     for i, ((_, blob), off, noff) in enumerate(zip(files, data_offsets,
                                                                    name_offsets)))
    return header + table + names + data


def make_dcx(payload: bytes) -> bytes:
    """A DCX with the header the game's archives have."""
    packed = zlib.compress(payload, 9)
    return (sb.DCX_HEADER.pack(b"DCX\0", 0x10000, 0x18, 0x24, 0x44, 0x4C, b"DCS\0", len(payload),
                               len(packed), b"DCP\0", b"DFLT", 0x20)
            + sb.DCX_DCP_REST + sb.DCX_DCA.pack(b"DCA\0", 8) + packed)


class DcxTests(unittest.TestCase):
    def test_roundtrip(self):
        payload = bytes(range(256)) * 10
        self.assertEqual(sb.unpack_dcx(make_dcx(payload)), payload)

    def test_bad_magic(self):
        with self.assertRaisesRegex(sb.FormatError, "not a DCX"):
            sb.unpack_dcx(b"DCY\0" + make_dcx(b"x")[4:])

    def test_size_mismatch(self):
        data = bytearray(make_dcx(b"hello"))
        struct.pack_into(">I", data, 0x1C, 6)
        with self.assertRaisesRegex(sb.FormatError, "header says"):
            sb.unpack_dcx(bytes(data))

    def test_truncated_stream(self):
        data = bytearray(make_dcx(bytes(1000))[:-2])
        struct.pack_into(">I", data, 0x20, len(data) - 0x4C)
        with self.assertRaisesRegex(sb.FormatError, "does not end"):
            sb.unpack_dcx(bytes(data))


class Bnd4Tests(unittest.TestCase):
    def test_entries(self):
        files = [("a.vpo", b"first"), ("Dir\\b.ppo", b"second!")]
        for fmt in sb.BND4_FORMATS:
            version, entries = sb.unpack_bnd4(make_bnd4(files, fmt))
            self.assertEqual(version, "10G29R1")
            self.assertEqual([(e.name, e.data) for e in entries], files)

    def test_unknown_variant(self):
        with self.assertRaisesRegex(sb.FormatError, "unsupported BND4"):
            sb.unpack_bnd4(make_bnd4([("a", b"x")], fmt=0x55))

    def test_data_out_of_range(self):
        data = bytearray(make_bnd4([("a", b"x")]))
        struct.pack_into("<I", data, 0x40 + 0x18, len(data))
        with self.assertRaisesRegex(sb.FormatError, "out of range"):
            sb.unpack_bnd4(bytes(data))

    def test_duplicate_name(self):
        with self.assertRaisesRegex(sb.FormatError, "duplicate"):
            sb.unpack_bnd4(make_bnd4([("a", b"x"), ("a", b"y")]))

    def test_unsafe_names(self):
        for name in ("..\\x", "a\\\\b", "a/b", ""):
            with self.subTest(name), self.assertRaisesRegex(sb.FormatError, "unsafe"):
                sb.entry_path(name)
        self.assertEqual(str(sb.entry_path("Dir\\b@[cl].ppo")), "Dir/b@[cl].ppo")


class SplitTests(unittest.TestCase):
    def test_one_program(self):
        entry, spans = make_shader_entry([b"\x01" * 12])
        self.assertEqual(sb.split_programs(entry), spans)
        self.assertEqual(spans[0][0], 0x50)

    def test_two_programs(self):
        entry, spans = make_shader_entry([b"\x01" * 40, b"\x02" * 8])
        self.assertEqual(sb.split_programs(entry), spans)
        offset, size = spans[0]
        self.assertTrue(entry[offset:offset + size].endswith(footer_block(44, 0)))
        self.assertTrue(entry.endswith(b"(no_name)\0"))
        self.assertEqual(sum(size for _, size in spans), len(entry) - 0x50)

    def test_no_shdr_header(self):
        entry, _ = make_shader_entry([b"\x01" * 4])
        with self.assertRaisesRegex(sb.FormatError, "Shdr"):
            sb.split_programs(bytes(0x24) + b"Shdx" + entry[0x28:])

    def test_fixed_header_fields(self):
        entry, _ = make_shader_entry([b"\x01" * 4])
        for at, value in sb.SHDR_FIXED:
            data = bytearray(entry)
            data[at] ^= 1
            with self.subTest(at=at), self.assertRaisesRegex(sb.FormatError, f"field at {at:#x}"):
                sb.split_programs(bytes(data))

    def test_no_footer(self):
        entry, _ = make_shader_entry([b"\x01" * 4])
        with self.assertRaisesRegex(sb.FormatError, "no OrbShdr"):
            sb.split_programs(entry.replace(b"OrbShdr", b"OrbShdx"))

    def test_empty_code(self):
        entry, _ = make_shader_entry([b"\x01" * 4], pre=b"")
        with self.assertRaisesRegex(sb.FormatError, "no code"):
            sb.split_programs(entry[:0x50] + entry[0x50 + 8:])


class FooterTests(unittest.TestCase):
    def program(self, **kwargs) -> bytes:
        entry, spans = make_shader_entry([b"\x01" * 12, b"\x02" * 8], types=[5, 6], **kwargs)
        offset, size = spans[0]
        return entry[offset:offset + size]

    def test_fields(self):
        footer = sb.read_footer(self.program(), ENTRY_HASH)
        code = b"\x01" * 12 + S_ENDPGM
        self.assertEqual(footer, {"type": 5, "stage": "gs", "code_size": 16,
                                  "code_sha256": hashlib.sha256(code).hexdigest()})

    def test_stages(self):
        self.assertEqual(sb.STAGES, ("ps", "vs", "es", "ls", "cs", "gs", "vs", "hs", "vs"))

    def test_code_size_must_end_after_s_endpgm(self):
        program = bytearray(self.program())
        struct.pack_into("<I", program, 12 + 4 + 4 + 8, 12 << 8 | 0x57)
        with self.assertRaisesRegex(sb.FormatError, "s_endpgm"):
            sb.read_footer(bytes(program), ENTRY_HASH)

    def test_unknown_fields_refused(self):
        block = 12 + 4 + 4
        for at, value in ((7, 6), (8, 16 << 8 | 0x57 & ~1), (8, 16 << 8 | 9 << 2 | 0x43),
                          (0x14, 1)):
            program = bytearray(self.program())
            if at == 7:
                program[block + 7] = value
            else:
                struct.pack_into("<I", program, block + at, value)
            with self.subTest(at=at, value=value), \
                    self.assertRaisesRegex(sb.FormatError, "unsupported footer"):
                sb.read_footer(bytes(program), ENTRY_HASH)

    def test_hash_must_match_entry(self):
        with self.assertRaisesRegex(sb.FormatError, "differs"):
            sb.read_footer(self.program(), ENTRY_HASH + 1)


class MetadataTests(unittest.TestCase):
    EXPECTED = {
        "resources": [{"name": "cbParam", "slot": 3, "kind": "16046a00"},
                      {"name": "g_Texture0", "slot": 1, "kind": "02050300"}],
        "samplers": [{"name": "SS_ClampLinear", "slot": 2}],
        "inputs": [{"name": "IN.tex0", "semantic": "TEXCOORD", "index": 1, "kind": "03240100"}],
        "outputs": [{"name": "Main", "semantic": "(no_name)", "index": 0, "kind": "03380000"}],
    }

    def test_names(self):
        for gap in (b"", bytes(0x40)):
            entry, spans = make_shader_entry([b"\x01" * 4], gap=gap)
            end = spans[0][0] + 4 + 4 + 4 + sb.ORBSHDR_BLOCK
            with self.subTest(gap=len(gap)):
                self.assertEqual(sb.read_metadata(entry, end), self.EXPECTED)
                with self.assertRaisesRegex(sb.FormatError, "does not follow"):
                    sb.read_metadata(entry, end + 16)

    def test_record_count_mismatch(self):
        entry, _ = make_shader_entry([b"\x01" * 4])
        meta = 0x24 + struct.unpack_from("<I", entry, 0x10)[0]
        data = bytearray(entry)
        data[meta + 0x10] = 2  # two inputs
        with self.assertRaisesRegex(sb.FormatError, "do not end"):
            sb.read_metadata(bytes(data), meta)

    def test_name_outside_pool(self):
        entry, _ = make_shader_entry([b"\x01" * 4])
        meta = 0x24 + struct.unpack_from("<I", entry, 0x10)[0]
        data = bytearray(entry)
        struct.pack_into("<I", data, meta + 0x1C + 0x14, 0)  # cbParam's name points to itself
        with self.assertRaisesRegex(sb.FormatError, "outside the name pool"):
            sb.read_metadata(bytes(data), meta)


    def test_unknown_fields_refused(self):
        entry, _ = make_shader_entry([b"\x01" * 4])
        meta = 0x24 + struct.unpack_from("<I", entry, 0x10)[0]
        res = meta + sb.META_HEADER.size
        samp = res + 2 * sb.RESOURCE.size + sb.CONSTANT_SIZE
        var = samp + sb.SAMPLER.size
        for at, value, error in (
                (res + 0x0C, 0, "member range 0\\+0 of 1"),  # constant buffer without members
                (res + 0x0C, 2, "member range 0\\+2 of 1"),  # more members than constants
                (res + 0x10, 1, "member range 1\\+1 of 1"),
                (res + sb.RESOURCE.size + 0x0C, 1, "resource 1 .kind 02050300."),  # texture
                (res + sb.RESOURCE.size + 0x10, 1, "resource 1"),
                (samp + 4, 1, "sampler 0"),
                (var + sb.VARYING.size + 4, 1, "input/output 1")):
            data = bytearray(entry)
            struct.pack_into("<I", data, at, value)
            with self.subTest(at=at - meta, value=value), \
                    self.assertRaisesRegex(sb.FormatError, error):
                sb.read_metadata(bytes(data), meta)


class ScanTests(unittest.TestCase):
    def test_counts(self):
        elf = bytearray(Builder().build())
        elf[0xE8:0xF4] = footer_block(16, 3)[:12]  # after the program headers, in PT_LOAD (R+X)
        elf += footer_block(8, 4)[:8] + struct.pack("<I", 8 << 8 | 4 << 2 | 0x41)
        elf += b"OrbShdr\x07" + bytes(4)
        self.assertEqual(sb.scan_eboot(bytes(elf)),
                         {("text", "ls", 3): 1, ("other", "cs", 1): 1, ("other", "invalid", 0): 1})


def synthetic_archive() -> tuple[bytes, dict[str, bytes]]:
    single, _ = make_shader_entry([b"\x11" * 16])
    double, _ = make_shader_entry([b"\x21" * 24, b"\x22" * 4], types=[5, 6])
    files = {"GXOne.ppo": single, "Sub\\GXTwo.gpo": double}
    return make_dcx(make_bnd4(list(files.items()))), files


class MainTests(unittest.TestCase):
    def run_main(self, tmp: str, dcx: bytes, pinned: dict[str, str]) -> int:
        src = Path(tmp, "in", "gxtest.shaderbnd.dcx")
        src.parent.mkdir()
        src.write_bytes(dcx)
        saved = sb.SHADER_ARCHIVES.copy()
        sb.SHADER_ARCHIVES.clear()
        sb.SHADER_ARCHIVES.update(pinned)
        try:
            return sb.main([str(src), "-o", str(Path(tmp, "out"))])
        finally:
            sb.SHADER_ARCHIVES.clear()
            sb.SHADER_ARCHIVES.update(saved)

    def test_extract(self):
        dcx, files = synthetic_archive()
        pinned = {hashlib.sha256(dcx).hexdigest(): "gxtest.shaderbnd.dcx"}
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(self.run_main(tmp, dcx, pinned), 0)
            base = Path(tmp, "out", "gxtest.shaderbnd")
            index = json.loads((base / "index.json").read_text(encoding="utf-8"))
            self.assertEqual(index["entries"], 2)
            rows = index["programs"]
            self.assertEqual([(r["entry"], r["program"]) for r in rows],
                             [("GXOne.ppo", 0), ("Sub\\GXTwo.gpo", 0), ("Sub\\GXTwo.gpo", 1)])
            self.assertEqual((base / "entries/Sub/GXTwo.gpo").read_bytes(), files["Sub\\GXTwo.gpo"])
            for row in rows:
                blob = files[row["entry"]][row["offset"]:row["offset"] + row["size"]]
                self.assertEqual((base / row["file"]).read_bytes(), blob)
            self.assertEqual(rows[2]["file"], "programs/Sub/GXTwo.gpo.1")
            self.assertEqual([(r["stage"], r["code_size"]) for r in rows],
                             [("ps", 20), ("gs", 28), ("vs", 8)])
            self.assertEqual(rows[2]["code_sha256"],
                             hashlib.sha256(b"\x22" * 4 + S_ENDPGM).hexdigest())
            self.assertEqual(index["metadata"]["Sub\\GXTwo.gpo"], MetadataTests.EXPECTED)

    def test_unknown_archive_refused(self):
        dcx, _ = synthetic_archive()
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(self.run_main(tmp, dcx, {}), 1)
            self.assertFalse(Path(tmp, "out").exists())


# File and program counts per archive, from the table in docs/design/graphics.md.
EXPECTED = {
    "gxflvershader.shaderbnd.dcx": (14571, 16265),
    "gxflvershader.shaderbnddebug.dcx": (3194, 3194),
    "gxffxshader.shaderbnd.dcx": (301, 308),
    "gxdecal.shaderbnd.dcx": (193, 193),
    "gxposteffect.shaderbnd.dcx": (119, 119),
    "gxrenderershader.shaderbnd.dcx": (111, 111),
    "gxshader.shaderbnd.dcx": (15, 15),
    "gxgui.shaderbnd.dcx": (13, 13),
}

# Programs per (stage, type) over all 8 archives (docs/formats/shader-binary.md).
EXPECTED_STAGES = {("ps", 0): 3209, ("vs", 1): 10116, ("es", 2): 1699, ("ls", 3): 850,
                   ("cs", 4): 69, ("gs", 5): 1701, ("vs", 6): 1701, ("hs", 7): 404,
                   ("vs", 8): 469}

GAME_DIR = sb.load_game_dir()
EBOOT_ELF = sb.ELF_DIR / "eboot.elf"


@unittest.skipUnless(GAME_DIR, "BB_GAME_DIR not set (environment or local.env)")
class GameTests(unittest.TestCase):
    def test_all_archives(self):
        total = 0
        stages = Counter()
        for path in sb.default_inputs(GAME_DIR):
            with self.subTest(path.name):
                name, data = read_pinned(path, sb.SHADER_ARCHIVES)
                archive = sb.extract(data)
                self.assertEqual((len(archive.entries), len(archive.programs)), EXPECTED[name])
                total += len(archive.programs)
                stages.update((row["stage"], row["type"]) for row in archive.programs)
                self.assertEqual(len(archive.metadata), len(archive.entries))
                entries = {entry.name: entry.data for entry in archive.entries}
                for row in archive.programs:
                    if row["program"] > 0:  # the previous program's footer block ends here
                        block = row["offset"] - sb.ORBSHDR_BLOCK
                        self.assertEqual(entries[row["entry"]][block:block + 7], sb.ORBSHDR)
        self.assertEqual(total, 20218)
        self.assertEqual(stages, EXPECTED_STAGES)


@unittest.skipUnless(EBOOT_ELF.is_file(), "no eboot.elf from tools/self2elf in analysis/elf/")
class EbootScanTests(unittest.TestCase):
    def test_eboot(self):
        _, elf = read_pinned(EBOOT_ELF, sb.PINNED_ELFS)
        counts = sb.scan_eboot(elf)
        self.assertEqual(sum(counts.values()), 5493)
        stages = Counter()
        for (_, stage, _), n in counts.items():
            stages[stage] += n
        self.assertEqual(stages, {"ps": 5230, "vs": 140, "es": 37, "ls": 37, "cs": 49})


if __name__ == "__main__":
    unittest.main()
