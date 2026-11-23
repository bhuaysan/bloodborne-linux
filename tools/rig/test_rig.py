#!/usr/bin/env python3
"""Tests for the rig command line: sandbox, limits, targets and the layer build. Run: python3 -m unittest discover -s tools/rig

No test here starts the game or needs a GPU, /dev/uinput or systemd.
"""

from __future__ import annotations

import argparse
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

    def test_container_replaces_bwrap_and_mounts_the_game_read_only(self) -> None:
        prefix = rig.container_prefix("img", Path("/games/bb"), Path("/pkg"), Path("/runs/r"),
                                      {"HOME": "/h", "XDG_RUNTIME_DIR": "/rt"})
        self.assertEqual(prefix[:2], ["podman", "run"])
        self.assertEqual(prefix[-1], "img")
        self.assertIn("--cgroups=disabled", prefix)  # in the scope: limits and samplers reach it
        self.assertIn("/games/bb:/games/bb:ro", prefix)
        self.assertIn("/pkg:/pkg:ro", prefix)
        self.assertEqual(prefix[prefix.index("-w") + 1], "/runs/r")
        command = rig.wrap_command(["/pkg/bb-run", "-x"], Path("/games/bb"), "bb-rig-1", 600, "8G",
                                   prefix)
        self.assertNotIn("bwrap", command)
        self.assertIn("MemoryMax=8G", command)
        at = command.index("timeout")
        self.assertEqual(command[at:at + 4], ["timeout", "-k", "10", "600"])
        self.assertEqual(command[at + 4:], [*prefix, "/pkg/bb-run", "-x"])

    def test_container_needs_a_package_bb_run(self) -> None:
        base = dict(log=None, perf=False, frame_rate=None, bb_run_args=None, bb_run_config=None,
                    user_from=None, container="img")
        for bad in (dict(target="bb-run", bb_run=None),
                    dict(target="reference", bb_run=None, perf=True),
                    dict(target="bb-run", bb_run=Path("/pkg/bb-run"), perf=True)):
            with self.assertRaises(SystemExit):
                rig.check_target_options(argparse.Namespace(**{**base, **bad}))
        rig.check_target_options(argparse.Namespace(**{**base, "target": "bb-run",
                                                       "bb_run": Path("/pkg/bb-run")}))
        rig.check_target_options(argparse.Namespace(**{**base, "target": "reference",
                                                       "bb_run": None}))

    def test_reference_container_gets_the_input_devices(self) -> None:
        env = {"HOME": "/h", "XDG_RUNTIME_DIR": "/rt"}
        plain = rig.container_prefix("img", Path("/games/bb"), Path("/pkg"), Path("/r"), env)
        self.assertNotIn("/dev/input:/dev/input", plain)
        pad = rig.container_prefix("img", Path("/games/bb"), Path("/ref"), Path("/r"), env,
                                   input_devices=True)
        self.assertIn("/dev/input:/dev/input", pad)
        self.assertIn("/run/udev:/run/udev:ro", pad)
        self.assertIn("/ref:/ref:ro", pad)
        self.assertEqual(pad[-1], "img")

    def test_read_only_check_refuses_a_writable_game_dir(self) -> None:
        with unittest.mock.patch("subprocess.run") as run_mock:
            run_mock.return_value = subprocess.CompletedProcess([], 1)
            with self.assertRaises(SystemExit):
                rig.check_game_read_only(Path("/games/bb"))
            run_mock.return_value = subprocess.CompletedProcess([], 0)
            rig.check_game_read_only(Path("/games/bb"))
            rig.check_game_read_only(Path("/games/bb"), ["podman", "run", "img"])
            self.assertEqual(run_mock.call_args.args[0][:4], ["podman", "run", "img", "sh"])

    def test_kernel_log_failure_is_not_an_empty_log(self) -> None:
        with unittest.mock.patch("subprocess.run") as run_mock:
            run_mock.return_value = subprocess.CompletedProcess([], 1, stdout="", stderr="denied")
            self.assertIsNone(rig.kernel_journal(0))
            run_mock.return_value = subprocess.CompletedProcess([], 1, stdout="-- No entries --\n")
            self.assertIsNotNone(rig.kernel_journal(0))

    def _checkout(self, root: Path, commit_time: int) -> Path:
        """A git checkout at `root` whose last commit is at `commit_time`."""
        root.mkdir()
        # No inherited GIT_DIR or GIT_INDEX_FILE (a git hook sets them): never the real repository.
        env = {**{k: v for k, v in os.environ.items() if not k.startswith("GIT_")},
               "GIT_COMMITTER_DATE": f"{commit_time} +0000",
               "GIT_AUTHOR_DATE": f"{commit_time} +0000", "GIT_AUTHOR_NAME": "t",
               "GIT_AUTHOR_EMAIL": "t@example.invalid", "GIT_COMMITTER_NAME": "t",
               "GIT_COMMITTER_EMAIL": "t@example.invalid", "GIT_CONFIG_GLOBAL": os.devnull,
               "GIT_CONFIG_NOSYSTEM": "1"}
        for argv in (["git", "init", "-q"], ["git", "commit", "-q", "--allow-empty", "-m", "c"]):
            subprocess.run(argv, cwd=root, env=env, check=True, capture_output=True)
        return root

    def _build(self, checkout: Path, preset: str, mtime: int) -> Path:
        binary = checkout / "build" / preset / "src" / "tools" / "bb-run"
        binary.parent.mkdir(parents=True)
        binary.write_text("")
        os.utime(binary, (mtime, mtime))
        return binary

    def test_stale_bb_run_is_refused_unless_allowed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            here = self._checkout(Path(tmp) / "here", 2_000_000_000)
            binary = self._build(here, "debug", 2_000_000_000 - 60)
            with self.assertRaises(SystemExit) as refused:
                rig.locate_bb_run("debug", here=here, main=here)
            self.assertIn(str(binary), str(refused.exception))
            self.assertIn("--allow-stale", str(refused.exception))
            allowed = rig.locate_bb_run("debug", allow_stale=True, here=here, main=here)
            self.assertEqual(allowed, rig.BbRunBuild(binary, False, True))

    def test_current_bb_run_is_accepted(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            here = self._checkout(Path(tmp) / "here", 2_000_000_000)
            binary = self._build(here, "debug", 2_000_000_000 + 60)
            self.assertEqual(rig.locate_bb_run("debug", here=here, main=here),
                             rig.BbRunBuild(binary, False, False))

    def test_main_checkout_fallback_is_reported_and_checked(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            here = self._checkout(Path(tmp) / "here", 2_000_000_000)
            main = self._checkout(Path(tmp) / "main", 1_900_000_000)
            # Newer than the main checkout's last commit, older than this one's: still stale.
            binary = self._build(main, "debug", 1_950_000_000)
            with self.assertRaises(SystemExit):
                rig.locate_bb_run("debug", here=here, main=main)
            os.utime(binary, (2_000_000_060, 2_000_000_060))
            self.assertEqual(rig.locate_bb_run("debug", here=here, main=main),
                             rig.BbRunBuild(binary, True, False))
            # The worktree's own build wins over the main checkout's.
            own = self._build(here, "debug", 2_000_000_060)
            self.assertEqual(rig.locate_bb_run("debug", here=here, main=main).path, own)

    def test_no_bb_run_build_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            here = self._checkout(Path(tmp) / "here", 2_000_000_000)
            with self.assertRaises(SystemExit):
                rig.locate_bb_run("debug", here=here, main=here)

    def test_stale_bb_run_stops_the_run_before_it_starts(self) -> None:
        args = argparse.Namespace(target="bb-run", scenario=None, no_layer=False, time=None,
                                  memory=None, preset="debug", bb_run=None, allow_stale=False)
        with unittest.mock.patch.object(rig.toolenv, "load_game_dir", return_value=Path("/g")), \
                unittest.mock.patch.object(rig.toolenv, "read_pinned"), \
                unittest.mock.patch.object(rig, "locate_bb_run",
                                           side_effect=SystemExit("stale")) as locate, \
                unittest.mock.patch.object(rig, "check_game_read_only") as checked, \
                unittest.mock.patch.object(rig, "rig_dir") as runs:
            with self.assertRaises(SystemExit):
                rig.run_target(args)
        locate.assert_called_once_with("debug", False)
        checked.assert_not_called()
        runs.assert_not_called()

    def test_bb_run_uses_a_per_run_user_dir_and_no_window(self) -> None:
        with unittest.mock.patch.object(rig, "find_bb_run", return_value=Path("/b/bb-run")):
            program = rig.bb_run_program(Path("/run"), Path("/g"), "debug", False, Path("/w"))
        self.assertEqual(program.env["BB_USER_DIR"], "/w/user")
        self.assertEqual(rig.bb_run_profile_data(Path("/w")), Path("/w/user/profiles/rig"))
        self.assertEqual(program.env["SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS"], "1")
        self.assertEqual(program.argv[:3], ["/b/bb-run", "--game-dir", "/g"])
        self.assertIn("--headless", program.argv)
        with unittest.mock.patch.object(rig, "find_bb_run", return_value=Path("/b/bb-run")):
            windowed = rig.bb_run_program(Path("/run"), Path("/g"), "debug", True, Path("/w"))
        self.assertNotIn("--headless", windowed.argv)

    def test_bb_run_config_replaces_game_dir_and_profile(self) -> None:
        with unittest.mock.patch.object(rig, "find_bb_run", return_value=Path("/b/bb-run")):
            program = rig.bb_run_program(Path("/run"), Path("/g"), "debug", False, Path("/w"),
                                         config=Path("/c.toml"))
        self.assertEqual(program.argv[:3], ["/b/bb-run", "--config", "/c.toml"])
        self.assertNotIn("--game-dir", program.argv)
        self.assertNotIn("--profile", program.argv)

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

    def test_bb_run_extra_args_are_split_and_appended(self) -> None:
        with unittest.mock.patch.object(rig, "find_bb_run", return_value=Path("/b/bb-run")):
            plain = rig.bb_run_program(Path("/run"), Path("/g"), "debug", True, Path("/w"))
            extra = rig.bb_run_program(Path("/run"), Path("/g"), "debug", True, Path("/w"),
                                       frame_rate=60,
                                       extra_args="--window-size 2560x1440 --fsr  --fullscreen")
        self.assertEqual(extra.argv[:len(plain.argv)], plain.argv)
        self.assertEqual(extra.argv[-6:], ["--frame-rate", "60", "--window-size", "2560x1440",
                                           "--fsr", "--fullscreen"])

    def test_layer_path_keeps_other_layers_findable(self) -> None:
        plain = rig.layer_path(Path("/rig/layer"), {"XDG_DATA_HOME": "/d"}).split(":")
        self.assertEqual(plain[0], "/rig/layer")
        self.assertEqual(plain[1:], [*rig.SYSTEM_LAYER_DIRS, "/d/vulkan/explicit_layer.d"])
        given = rig.layer_path(Path("/rig/layer"), {"VK_LAYER_PATH": "/a:/b",
                                                    "VK_ADD_LAYER_PATH": "/c",
                                                    "XDG_DATA_HOME": "/d"}).split(":")
        self.assertEqual(given[:4], ["/rig/layer", "/a", "/b", "/c"])

    def test_reference_refuses_bb_run_options(self) -> None:
        for options in ({"frame_rate": 60}, {"frame_rate": 30}, {"log": "profile=debug"},
                        {"perf": True}, {"bb_run_args": "--fsr"}, {"bb_run_config": "/c.toml"}):
            args = rig.argparse.Namespace(target="reference", **options)
            with self.assertRaises(SystemExit, msg=str(options)):
                rig.check_target_options(args)
            rig.check_target_options(rig.argparse.Namespace(target="bb-run", **options))
        rig.check_target_options(rig.argparse.Namespace(target="reference", frame_rate=None,
                                                        log=None, perf=False, bb_run_args=None))

    def test_bb_run_config_refuses_user_from(self) -> None:
        args = rig.argparse.Namespace(target="bb-run", bb_run_config="/c.toml", user_from=Path("/u"))
        with self.assertRaises(SystemExit) as caught:
            rig.check_target_options(args)
        self.assertIn("--user-from", str(caught.exception))
        rig.check_target_options(rig.argparse.Namespace(target="bb-run", bb_run_config="/c.toml",
                                                        user_from=None))
        rig.check_target_options(rig.argparse.Namespace(target="bb-run", bb_run_config=None,
                                                        user_from=Path("/u")))

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
        self.assertNotIn("BB_CACHE_DIR", cold.env)
        self.assertEqual(cold.env["BB_USER_DIR"], "/w/user")
        self.assertEqual(warm.env["BB_CACHE_DIR"], "/c")

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


class MapCountTests(unittest.TestCase):
    def test_the_largest_process_counts_and_gone_ones_are_skipped(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            proc = Path(tmp)
            for pid, lines in (("10", 3), ("11", 70000)):
                (proc / pid).mkdir()
                (proc / pid / "maps").write_text("00400000-00401000 r-xp 0 0:0 0\n" * lines)
            self.assertEqual(rig.count_maps(["10", "11", "12"], proc), 70000)
            self.assertEqual(rig.count_maps([], proc), 0)

    def test_own_process_has_mappings(self) -> None:
        self.assertGreater(rig.count_maps([str(os.getpid())]), 0)


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


    def test_portable_layer_is_built_in_the_sniper_sdk(self) -> None:
        with tempfile.TemporaryDirectory() as temp, \
                unittest.mock.patch("subprocess.run") as run_mock:
            rig.build_layer(Path(temp) / "layer", portable=True)
            command = run_mock.call_args.args[0]
            self.assertEqual(command[:2], ["podman", "run"])
            self.assertIn(rig.SNIPER_SDK, command)
            self.assertIn("-static-libstdc++", command)
            self.assertEqual(command[command.index(rig.SNIPER_SDK) + 1], "g++-14")

    def test_sniper_sdk_is_the_package_build_image(self) -> None:
        script = (HERE.parent / "release" / "build-package.sh").read_text()
        self.assertIn(rig.SNIPER_SDK, script)


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
