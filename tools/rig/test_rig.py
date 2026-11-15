#!/usr/bin/env python3
"""Tests for the rig command line: sandbox, limits, targets and the layer build. Run: python3 -m unittest discover -s tools/rig

No test here starts the game or needs a GPU, /dev/uinput or systemd.
"""

from __future__ import annotations

import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import unittest.mock
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import rig  # noqa: E402
import vpad  # noqa: E402


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

    def test_bb_run_log_filter_is_passed_on(self) -> None:
        with unittest.mock.patch.object(rig, "find_bb_run", return_value=Path("/b/bb-run")):
            plain = rig.bb_run_program(Path("/run"), Path("/g"), "debug", False, Path("/w"))
            logged = rig.bb_run_program(Path("/run"), Path("/g"), "debug", False, Path("/w"),
                                        log="profile=debug")
        self.assertNotIn("--log", plain.argv)
        self.assertEqual(logged.argv[-2:], ["--log", "profile=debug"])

    def test_bb_run_frame_rate_is_passed_on(self) -> None:
        with unittest.mock.patch.object(rig, "find_bb_run", return_value=Path("/b/bb-run")):
            plain = rig.bb_run_program(Path("/run"), Path("/g"), "debug", False, Path("/w"))
            sixty = rig.bb_run_program(Path("/run"), Path("/g"), "debug", False, Path("/w"),
                                       frame_rate=60)
        self.assertNotIn("--frame-rate", plain.argv)
        self.assertEqual(sixty.argv[-2:], ["--frame-rate", "60"])

    def test_reference_refuses_bb_run_options(self) -> None:
        for options in ({"frame_rate": 60}, {"frame_rate": 30}, {"log": "profile=debug"},
                        {"perf": True}):
            args = rig.argparse.Namespace(target="reference", **options)
            with self.assertRaises(SystemExit, msg=str(options)):
                rig.check_target_options(args)
            rig.check_target_options(rig.argparse.Namespace(target="bb-run", **options))
        rig.check_target_options(rig.argparse.Namespace(target="reference", frame_rate=None,
                                                        log=None, perf=False))

    def test_bb_run_under_perf_record(self) -> None:
        with unittest.mock.patch.object(rig, "find_bb_run", return_value=Path("/b/bb-run")):
            program = rig.bb_run_program(Path("/run"), Path("/g"), "debug", False, Path("/w"),
                                         perf=True)
        self.assertEqual(program.argv[:2], ["perf", "record"])
        self.assertIn("/run/perf.data", program.argv)
        self.assertEqual(program.argv[program.argv.index("--") + 1], "/b/bb-run")

    def test_reference_sees_only_the_virtual_pad_and_knows_its_mapping(self) -> None:
        # A real controller on the machine took pad slot 0 in most runs (#520).
        description = vpad.device_description()
        vendor, product = struct.unpack_from("HH", description, 82)
        allowed = rig.PAD_ONLY_VIRTUAL["SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT"]
        self.assertEqual(allowed, f"0x{vendor:04x}/0x{product:04x}")
        self.assertEqual(rig.PAD_ONLY_VIRTUAL["SDL_GAMECONTROLLERCONFIG"], vpad.sdl_mapping())
        with tempfile.TemporaryDirectory() as temp, \
                unittest.mock.patch.object(rig, "reference_root", return_value=Path(temp)), \
                unittest.mock.patch.object(rig, "sha256", return_value=rig.REFERENCE_BIN_SHA256):
            binary = Path(temp) / "squashfs-root" / "usr" / "bin" / "shadps4"
            binary.parent.mkdir(parents=True)
            binary.touch()
            run = Path(temp) / "run"
            run.mkdir()
            program = rig.reference_program(run, Path("/g"), None)
        for name, value in rig.PAD_ONLY_VIRTUAL.items():
            self.assertEqual(program.env[name], value)

    def test_bb_run_gets_its_pad_in_process_not_from_the_environment(self) -> None:
        with unittest.mock.patch.object(rig, "find_bb_run", return_value=Path("/b/bb-run")):
            program = rig.bb_run_program(Path("/run"), Path("/g"), "debug", True, Path("/w"),
                                         pad_input=Path("/run/pad"))
            without = rig.bb_run_program(Path("/run"), Path("/g"), "debug", True, Path("/w"))
        self.assertEqual(program.argv[program.argv.index("--pad-input") + 1], "/run/pad")
        self.assertNotIn("--pad-input", without.argv)
        for name in rig.PAD_ONLY_VIRTUAL:
            self.assertNotIn(name, program.env)

    def test_stream_pad_writes_to_the_fifo(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            fifo = Path(temp) / "pad"
            os.mkfifo(fifo)
            reader = os.open(fifo, os.O_RDONLY | os.O_NONBLOCK)
            try:
                pad = rig.open_stream_pad(fifo)
                pad._sleep = lambda s: None
                pad.tap("START")
                pad.close()
                self.assertEqual(os.read(reader, 4096).decode().splitlines(),
                                 ["button START 1", "button START 0", "neutral"])
            finally:
                os.close(reader)

    def test_bb_run_cache_is_cold_unless_asked(self) -> None:
        with unittest.mock.patch.object(rig, "find_bb_run", return_value=Path("/b/bb-run")):
            cold = rig.bb_run_program(Path("/run"), Path("/g"), "debug", False, Path("/w"))
            warm = rig.bb_run_program(Path("/run"), Path("/g"), "debug", False, Path("/w"),
                                      Path("/c"))
        self.assertEqual(cold.env["XDG_CACHE_HOME"], "/w/cache")
        self.assertEqual(warm.env["XDG_CACHE_HOME"], "/c")

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


class PerfRunsTests(unittest.TestCase):
    def test_runs_each_column_n_times_and_passes_the_binary(self) -> None:
        calls = []

        def fake(run_args):
            calls.append((run_args.target, run_args.user_from, run_args.bb_run,
                          run_args.frame_rate))
            run_args.run_dir_out.append(Path(f"run{len(calls)}"))
            return 0

        args = rig.argparse.Namespace(
            scenario="s", runs=2, bb_user=Path("bu"), reference_user=Path("ru"),
            main_bb_run=Path("/main/bb-run"), time=None, preset="relwithdebinfo", frame_rate=60)
        with unittest.mock.patch.object(rig, "run_target", fake):
            dirs = rig.perf_runs(args)
        self.assertEqual(list(dirs), ["branch", "main", "reference"])
        self.assertTrue(all(len(v) == 2 for v in dirs.values()))
        # The frame rate goes to both bb-run columns, never to the reference.
        self.assertEqual(calls, [("bb-run", Path("bu"), None, 60)] * 2
                         + [("bb-run", Path("bu"), Path("/main/bb-run"), 60)] * 2
                         + [("reference", Path("ru"), None, None)] * 2)

    def test_needs_a_scenario_and_a_save(self) -> None:
        args = rig.argparse.Namespace(
            scenario="s", runs=1, bb_user=None, reference_user=Path("ru"),
            main_bb_run=None, time=None, preset="p", frame_rate=None)
        with self.assertRaises(SystemExit):
            rig.perf_runs(args)
        args.scenario = None
        with self.assertRaises(SystemExit):
            rig.perf_runs(args)


class UserCopyTests(unittest.TestCase):
    def test_reference_layout_is_converted_for_bb_run(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "user"
            save = src / "home" / "1000" / "savedata" / "CUSA00207" / "SPRJ0005"
            (save / "sce_sys").mkdir(parents=True)
            (save / "userdata0001").write_bytes(b"a")
            (save / "sce_sys" / "param.sfo").write_bytes(b"x")
            dest = Path(tmp) / "out"
            rig.copy_user_for_bb_run(src, dest)
            self.assertEqual((dest / "savedata" / "SPRJ0005" / "userdata0001").read_bytes(), b"a")
            self.assertFalse((dest / "savedata" / "SPRJ0005" / "sce_sys").exists())

    def test_bb_run_layout_is_copied_as_is(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "user"
            (src / "savedata" / "SPRJ0005").mkdir(parents=True)
            (src / "savedata" / "SPRJ0005" / "userdata0001").write_bytes(b"b")
            dest = Path(tmp) / "out"
            rig.copy_user_for_bb_run(src, dest)
            self.assertEqual((dest / "savedata" / "SPRJ0005" / "userdata0001").read_bytes(), b"b")


if __name__ == "__main__":
    unittest.main()
