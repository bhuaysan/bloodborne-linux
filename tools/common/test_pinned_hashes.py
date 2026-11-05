"""Checks that every copy of the pinned SHA-256 hashes agrees.

The hash of eboot.bin is in docs/clean-room.md, tools/self2elf/self2elf.py and
src/loader/self.hpp. The hash of the eboot.elf that self2elf rebuilds is in tools/common/ps4elf.py
and tests/loader/self_test.cpp. Needs no game files.
"""

import re
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(REPO / "tools" / "self2elf"))
import ps4elf  # noqa: E402
import self2elf  # noqa: E402

HEX = r"[0-9a-f]{64}"


def read(relative: str) -> str:
    return (REPO / relative).read_text(encoding="utf-8")


def by_name(pinned: dict[str, str]) -> dict[str, str]:
    return {name: digest for digest, name in pinned.items()}


class PinnedHashesTest(unittest.TestCase):
    def test_game_files_match_clean_room_table(self):
        rows = re.findall(rf"^\| `([^`]+)`[^|]*\| `({HEX})` \|$", read("docs/clean-room.md"),
                          re.MULTILINE)
        self.assertTrue(rows, "no hash table in docs/clean-room.md")
        # Compare the rows as a list, so a duplicate or extra row fails instead of being merged.
        known = [(name, digest) for digest, name in self2elf.KNOWN_FILES.items()]
        self.assertEqual(sorted(rows), sorted(known))

    def test_eboot_bin_in_loader(self):
        found = re.findall(rf'eboot_bin_sha256 =\s*"({HEX})"', read("src/loader/self.hpp"))
        self.assertEqual(found, [by_name(self2elf.KNOWN_FILES)["eboot.bin"]])

    def test_eboot_elf_in_loader_test(self):
        found = re.findall(rf'"({HEX})"', read("tests/loader/self_test.cpp"))
        self.assertEqual(found, [by_name(ps4elf.PINNED_ELFS)["eboot.elf"]])


if __name__ == "__main__":
    unittest.main()
