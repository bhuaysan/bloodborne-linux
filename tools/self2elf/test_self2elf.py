"""Tests for self2elf. Run: python3 -m unittest discover -s tools/self2elf

The synthetic tests need no game files. The game test runs only when BB_GAME_DIR is set
(environment or local.env).
"""

import hashlib
import struct
import tempfile
import unittest
from pathlib import Path

import self2elf as s2e


def build_self(trailer_bytes: bool = True, props_extra: int = 0, corrupt: bool = False) -> bytes:
    """Build a minimal SELF: one PT_LOAD with data, one digest-table entry, one SCE_LIBVERSION."""
    code = bytes(range(256)) * 4
    libver = b"libversion"
    phdrs = [
        s2e.Phdr(1, 5, 0x1000, 0, 0, len(code), len(code), 0x1000),
        s2e.Phdr(s2e.PT_SCE_LIBVERSION, 0, 0x1000 + len(code), 0, 0, len(libver), 0, 0x10),
    ]
    elf_hdr = s2e.ELF_HEADER.pack(b"\x7fELF\x02\x01\x01\x09" + bytes(8), 0xFE10, 0x3E, 1, 0,
                                  0x40, 0, 0, 0x40, 0x38, len(phdrs), 0, 0, 0)
    elf_head = elf_hdr + b"".join(s2e.ELF_PHDR.pack(*vars(p).values()) for p in phdrs)

    elf = bytearray(phdrs[1].p_offset + len(libver))
    elf[: len(elf_head)] = elf_head
    elf[0x1000: 0x1000 + len(code)] = code
    elf[phdrs[1].p_offset:] = libver
    digest = hashlib.sha256(elf).digest()

    num_entries = 2
    head = 0x20 + num_entries * 0x20 + len(elf_head)
    head = (head + 15) & ~15
    ext = s2e.EXT_INFO.pack(0x3100000000000001, 1, 0, 0, digest)
    header_size = head + len(ext)
    meta_size = 0x40
    table_off = header_size + meta_size
    data_off = table_off + 0x20
    file_size = data_off + len(code)

    entries = [
        s2e.SELF_ENTRY.pack((1 << 20) | s2e.PROP_BLOCK_TABLE | 4, table_off, 0x20, 0x20),
        s2e.SELF_ENTRY.pack((0 << 20) | 0x2804 | props_extra, data_off, len(code), len(code)),
    ]
    out = bytearray(s2e.SELF_HEADER.pack(s2e.SELF_MAGIC, s2e.SELF_CONSTANT, 0x101, header_size,
                                         meta_size, file_size, 0, num_entries, 0x22, 0))
    out += b"".join(entries) + elf_head
    out += bytes(head - len(out)) + ext + bytes(meta_size) + bytes(0x20) + code
    if corrupt:
        out[data_off] ^= 0xFF
    if trailer_bytes:
        out += libver
    return bytes(out)


class SyntheticTests(unittest.TestCase):
    def test_roundtrip(self):
        image = s2e.self_to_elf(build_self())
        self.assertEqual(image.e_type, 0xFE10)
        self.assertEqual(image.data[0x1000:0x1004], bytes([0, 1, 2, 3]))
        self.assertTrue(image.data.endswith(b"libversion"))

    def test_missing_trailer(self):
        with self.assertRaisesRegex(s2e.SelfError, "no data for program header 1"):
            s2e.self_to_elf(build_self(trailer_bytes=False))

    def test_encrypted_segment(self):
        with self.assertRaisesRegex(s2e.SelfError, "encrypted or compressed"):
            s2e.self_to_elf(build_self(props_extra=s2e.PROP_ENCRYPTED))

    def test_digest_mismatch(self):
        with self.assertRaisesRegex(s2e.SelfError, "does not match the digest"):
            s2e.self_to_elf(build_self(corrupt=True))

    def test_bad_magic(self):
        with self.assertRaisesRegex(s2e.SelfError, "not a SELF"):
            s2e.self_to_elf(b"\x7fELF" + bytes(0x100))

    def test_unknown_build_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp, "in", "eboot.bin")
            src.parent.mkdir()
            src.write_bytes(build_self())
            self.assertEqual(s2e.main([str(src), "-o", str(Path(tmp, "out"))]), 1)
            self.assertFalse(Path(tmp, "out").exists())

    def test_output_next_to_input_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp, "eboot.bin")
            src.write_bytes(build_self())
            self.assertEqual(s2e.main([str(src), "-o", str(Path(tmp, "sub"))]), 2)


GAME_DIR = s2e.load_game_dir()


@unittest.skipUnless(GAME_DIR, "BB_GAME_DIR not set (environment or local.env)")
class GameTests(unittest.TestCase):
    def test_all_bundled_selfs(self):
        game_dir = GAME_DIR
        files = s2e.default_inputs(game_dir)
        self.assertEqual(len(files), len(s2e.KNOWN_FILES))
        for path in files:
            with self.subTest(path.name):
                data = path.read_bytes()
                self.assertIn(hashlib.sha256(data).hexdigest(), s2e.KNOWN_FILES)
                s2e.self_to_elf(data)


if __name__ == "__main__":
    unittest.main()
