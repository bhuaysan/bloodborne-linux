"""Tests for the output-directory guard in cli.py."""

import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cli  # noqa: E402


class CheckedOutDirTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name).resolve()
        self.game = self.root / "game"
        (self.game / "sub").mkdir(parents=True)
        self.inputs = self.root / "in"
        self.inputs.mkdir()
        self.input_file = self.inputs / "eboot.elf"
        patcher = mock.patch.object(cli, "load_game_dir", return_value=self.game)
        patcher.start()
        self.addCleanup(patcher.stop)

    def check(self, out_dir, self2elf_dir=None):
        with mock.patch("sys.stderr"):
            return cli.checked_out_dir(out_dir, [self.input_file], self2elf_dir)

    def test_refuses_game_dir_and_below(self):
        self.assertIsNone(self.check(self.game))
        self.assertIsNone(self.check(self.game / "sub"))
        self.assertIsNone(self.check(self.game / "new"))

    def test_refuses_input_and_self2elf_dirs(self):
        self.assertIsNone(self.check(self.inputs))
        elf_dir = self.root / "elf"
        self.assertIsNone(self.check(elf_dir / "x", self2elf_dir=elf_dir))

    def test_allows_sibling(self):
        out = self.root / "out"
        self.assertEqual(self.check(out), out)
        self.assertEqual(self.check(self.root / "gamex"), self.root / "gamex")


if __name__ == "__main__":
    unittest.main()
