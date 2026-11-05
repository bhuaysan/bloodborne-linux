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
from pathlib import Path

import shaderbnd as sb
from toolenv import read_pinned

S_ENDPGM = bytes.fromhex("000081bf")


def make_shader_entry(codes: list[bytes], pre: bytes = b"\x6a\x02\x00\x08",
                      tail: bytes = b"g_Texture0\0") -> tuple[bytes, list[tuple[int, int]]]:
    """A shader entry with one program per code blob, and the expected (offset, size) list."""
    header = bytearray(0x50)
    header[0x24:0x28] = b"Shdr"
    struct.pack_into("<I", header, 0x3C, len(header) - 0x34)
    out = bytearray(header)
    spans = []
    for code in codes:
        start = len(out)
        block = b"OrbShdr\x07" + bytes(0x1C - 8)
        out += code + S_ENDPGM + pre + block
        spans.append(start)
    out += tail
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
        self.assertTrue(entry[offset:offset + size].endswith(b"OrbShdr\x07" + bytes(0x14)))
        self.assertTrue(entry.endswith(b"g_Texture0\0"))
        self.assertEqual(sum(size for _, size in spans), len(entry) - 0x50)

    def test_no_shdr_header(self):
        entry, _ = make_shader_entry([b"\x01" * 4])
        with self.assertRaisesRegex(sb.FormatError, "Shdr"):
            sb.split_programs(bytes(0x24) + b"Shdx" + entry[0x28:])

    def test_no_footer(self):
        entry, _ = make_shader_entry([b"\x01" * 4])
        with self.assertRaisesRegex(sb.FormatError, "no OrbShdr"):
            sb.split_programs(entry.replace(b"OrbShdr", b"OrbShdx"))

    def test_empty_code(self):
        entry, _ = make_shader_entry([b"\x01" * 4], pre=b"")
        with self.assertRaisesRegex(sb.FormatError, "no code"):
            sb.split_programs(entry[:0x50] + entry[0x50 + 8:])


def synthetic_archive() -> tuple[bytes, dict[str, bytes]]:
    single, _ = make_shader_entry([b"\x11" * 16])
    double, _ = make_shader_entry([b"\x21" * 24, b"\x22" * 4])
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

GAME_DIR = sb.load_game_dir()


@unittest.skipUnless(GAME_DIR, "BB_GAME_DIR not set (environment or local.env)")
class GameTests(unittest.TestCase):
    def test_all_archives(self):
        total = 0
        for path in sb.default_inputs(GAME_DIR):
            with self.subTest(path.name):
                name, data = read_pinned(path, sb.SHADER_ARCHIVES)
                archive = sb.extract(data)
                self.assertEqual((len(archive.entries), len(archive.programs)), EXPECTED[name])
                total += len(archive.programs)
                entries = {entry.name: entry.data for entry in archive.entries}
                for row in archive.programs:
                    if row["program"] > 0:  # the previous program's footer block ends here
                        block = row["offset"] - sb.ORBSHDR_BLOCK
                        self.assertEqual(entries[row["entry"]][block:block + 7], sb.ORBSHDR)
        self.assertEqual(total, 20218)

if __name__ == "__main__":
    unittest.main()
