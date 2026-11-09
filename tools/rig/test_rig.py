#!/usr/bin/env python3
"""Tests for the rig command line: sandbox, limits, targets and the layer build. Run: python3 -m unittest discover -s tools/rig

No test here starts the game or needs a GPU, /dev/uinput or systemd.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import unittest
import unittest.mock
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import rig  # noqa: E402


class CommandTests(unittest.TestCase):
    def test_game_is_read_only_and_limits_are_set(self) -> None:
        command = rig.wrap_command(["/bin/game", "-x"], Path("/games/bb"), "bb-rig-1", 600, "8G")
        self.assertEqual(command[:5], ["systemd-run", "--user", "--scope", "-q", "--unit=bb-rig-1"])
        self.assertIn("MemoryMax=8G", command)
        self.assertIn("MemorySwapMax=0", command)
        ro = command.index("--ro-bind")
        self.assertEqual(command[ro:ro + 3], ["--ro-bind", "/games/bb", "/games/bb"])
        at = command.index("timeout")
        self.assertEqual(command[at:], ["timeout", "-k", "10", "600", "/bin/game", "-x"])

    def test_read_only_check_refuses_a_writable_game_dir(self) -> None:
        with unittest.mock.patch("subprocess.run") as run_mock:
            run_mock.return_value = subprocess.CompletedProcess([], 0)
            with self.assertRaises(SystemExit):
                rig.check_game_read_only(Path("/games/bb"))
            run_mock.return_value = subprocess.CompletedProcess([], 1)
            rig.check_game_read_only(Path("/games/bb"))

    def test_bb_run_uses_per_run_xdg_dirs_and_no_window(self) -> None:
        with unittest.mock.patch.object(rig, "find_bb_run", return_value=Path("/b/bb-run")):
            program = rig.bb_run_program(Path("/run"), Path("/g"), "debug", False, Path("/w"))
        self.assertEqual(program.env["XDG_DATA_HOME"], "/w/data")
        self.assertEqual(program.env["XDG_STATE_HOME"], "/w/state")
        self.assertEqual(program.argv[:3], ["/b/bb-run", "--game-dir", "/g"])
        self.assertIn("--headless", program.argv)
        with unittest.mock.patch.object(rig, "find_bb_run", return_value=Path("/b/bb-run")):
            windowed = rig.bb_run_program(Path("/run"), Path("/g"), "debug", True, Path("/w"))
        self.assertNotIn("--headless", windowed.argv)

    def test_zenity_stub_answers_without_blocking(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            stub = Path(temp) / "zenity"
            stub.write_text(rig.ZENITY_STUB)
            stub.chmod(0o755)
            env = {"BB_RIG_DIR": temp, "PATH": "/usr/bin:/bin"}
            version = subprocess.run([stub, "--version"], capture_output=True, text=True, env=env)
            self.assertEqual((version.returncode, version.stdout.strip()), (0, "4.0.0"))
            answer = subprocess.run([stub, "--question", "--text", "Save Migration"],
                                    capture_output=True, text=True, env=env)
            self.assertEqual(answer.stdout.strip(), "Do nothing")
            self.assertIn("Save Migration", (Path(temp) / "zenity-calls.txt").read_text())


class ScenarioFileTests(unittest.TestCase):
    def test_shipped_scenarios_parse(self) -> None:
        paths = sorted((HERE / "scenarios").glob("*.scn"))
        self.assertTrue(paths)
        for path in paths:
            with self.subTest(path.name):
                self.assertTrue(rig.scn.parse(path.read_text()).steps)


class LayerTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("g++") and Path("/usr/include/vulkan/vk_layer.h").exists(),
                         "needs g++ and the Vulkan headers")
    def test_layer_builds_cleanly_and_exports_the_negotiate_call(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            layer = rig.build_layer(Path(temp) / "layer")
            binary = layer / "libVkLayer_bb_rig.so"
            self.assertTrue(binary.is_file())
            self.assertTrue((layer / "VkLayer_bb_rig.json").is_file())
            symbols = subprocess.run(["nm", "-D", "--defined-only", str(binary)],
                                     capture_output=True, text=True, check=True).stdout
            self.assertIn("vkNegotiateLoaderLayerInterfaceVersion", symbols)


if __name__ == "__main__":
    unittest.main()
