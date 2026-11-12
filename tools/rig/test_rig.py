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
        self.assertLess(command.index("--dev-bind"), command.index("--ro-bind"))
        self.assertLess(command.index("--ro-bind"), command.index("timeout"))
        ro = command.index("--ro-bind")
        self.assertEqual(command[ro:ro + 3], ["--ro-bind", "/games/bb", "/games/bb"])
        at = command.index("timeout")
        self.assertEqual(command[at:], ["timeout", "-k", "10", "600", "/bin/game", "-x"])

    def test_read_only_check_refuses_a_writable_game_dir(self) -> None:
        with unittest.mock.patch("subprocess.run") as run_mock:
            run_mock.return_value = subprocess.CompletedProcess([], 1)
            with self.assertRaises(SystemExit):
                rig.check_game_read_only(Path("/games/bb"))
            run_mock.return_value = subprocess.CompletedProcess([], 0)
            rig.check_game_read_only(Path("/games/bb"))

    def test_kernel_log_failure_is_not_an_empty_log(self) -> None:
        with unittest.mock.patch("subprocess.run") as run_mock:
            run_mock.return_value = subprocess.CompletedProcess([], 1, stdout="", stderr="denied")
            self.assertIsNone(rig.kernel_journal(0))
            run_mock.return_value = subprocess.CompletedProcess([], 1, stdout="-- No entries --\n")
            self.assertIsNotNone(rig.kernel_journal(0))

    def test_bb_run_uses_per_run_xdg_dirs_and_no_window(self) -> None:
        with unittest.mock.patch.object(rig, "find_bb_run", return_value=Path("/b/bb-run")):
            program = rig.bb_run_program(Path("/run"), Path("/g"), "debug", False, Path("/w"))
        self.assertEqual(program.env["XDG_DATA_HOME"], "/w/data")
        self.assertEqual(program.env["XDG_STATE_HOME"], "/w/state")
        self.assertEqual(program.env["SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS"], "1")
        self.assertEqual(program.argv[:3], ["/b/bb-run", "--game-dir", "/g"])
        self.assertIn("--headless", program.argv)
        with unittest.mock.patch.object(rig, "find_bb_run", return_value=Path("/b/bb-run")):
            windowed = rig.bb_run_program(Path("/run"), Path("/g"), "debug", True, Path("/w"))
        self.assertNotIn("--headless", windowed.argv)

    def test_bb_run_cache_is_cold_unless_asked(self) -> None:
        with unittest.mock.patch.object(rig, "find_bb_run", return_value=Path("/b/bb-run")):
            cold = rig.bb_run_program(Path("/run"), Path("/g"), "debug", False, Path("/w"))
            warm = rig.bb_run_program(Path("/run"), Path("/g"), "debug", False, Path("/w"),
                                      Path("/c"))
            off = rig.bb_run_program(Path("/run"), Path("/g"), "debug", False, Path("/w"),
                                     pipeline_cache=False)
        self.assertEqual(cold.env["XDG_CACHE_HOME"], "/w/cache")
        self.assertEqual(warm.env["XDG_CACHE_HOME"], "/c")
        self.assertNotIn("--no-pipeline-cache", cold.argv)
        self.assertIn("--no-pipeline-cache", off.argv)

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


class OutputFailureTests(unittest.TestCase):
    STATS = {"frames": 5}

    def test_a_missing_shot_fails_the_run(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            run = Path(temp)
            (run / "shots").mkdir()
            (run / "shots" / "a.png").write_bytes(b"x")
            self.assertEqual(rig.missing_shots(run, ["a", "b", "b2"]), ["b", "b2"])
        failure = rig.output_failure(None, True, self.STATS, ["b"])
        self.assertIn("missing shots: b", failure)
        self.assertIsNone(rig.output_failure(None, True, self.STATS, []))

    def test_no_frames_fails_a_run_with_the_layer_only(self) -> None:
        self.assertIn("no frames", rig.output_failure(None, True, None, []))
        self.assertIsNone(rig.output_failure(None, False, None, []))

    def test_an_earlier_failure_stays(self) -> None:
        self.assertEqual(rig.output_failure("timed out", True, None, ["b"]), "timed out")

    def test_the_interpreter_lists_the_steps_it_finished(self) -> None:
        class Target:
            pad = None

            def alive(self) -> bool:
                return True

            def shot(self, name: str) -> None:
                if name == "bad":
                    raise rig.scn.StepFailed("no")

        interpreter = rig.scn.Interpreter(Target(), lambda kind, text: None)
        with self.assertRaises(rig.scn.StepFailed):
            interpreter.run(rig.scn.parse("shot a\nshot bad\nshot c\n"))
        self.assertEqual([s.args[0] for s in interpreter.executed], ["a"])


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
