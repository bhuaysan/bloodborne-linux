"""Tests for the commit subject length check in commit-msg and privacy-check.sh."""

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

HOOKS = Path(__file__).resolve().parent
SHELL = os.environ.get("TEST_SHELL", "sh")
PRIVACY_CHECK = HOOKS.parent / "privacy-check.sh"


def subject(length, prefix=""):
    return prefix + "x" * (length - len(prefix))


class CommitMsgTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.msg_file = Path(tmp.name) / "COMMIT_EDITMSG"

    def run_hook(self, text, env=None):
        self.msg_file.write_bytes(text.encode())
        return subprocess.run(
            [SHELL, str(HOOKS / "commit-msg"), str(self.msg_file)],
            capture_output=True, text=True, errors="replace", cwd=HOOKS,
            env={**os.environ, **(env or {})})

    def test_71_passes_and_72_fails(self):
        self.assertEqual(self.run_hook(subject(71) + "\n").returncode, 0)
        result = self.run_hook(subject(72) + "\n")
        self.assertEqual(result.returncode, 1)
        self.assertIn("72 characters", result.stderr)
        self.assertIn("under 72", result.stderr)

    def test_comment_lines_are_ignored(self):
        text = "# " + "c" * 100 + "\n" + subject(10) + "\n\n# more\n"
        self.assertEqual(self.run_hook(text).returncode, 0)
        text = "# comment\n" + subject(80) + "\n"
        self.assertEqual(self.run_hook(text).returncode, 1)

    def test_long_body_lines_are_fine(self):
        self.assertEqual(self.run_hook("Short\n\n" + "b" * 200 + "\n").returncode, 0)

    def test_exemptions(self):
        for prefix in ("fixup! ", "squash! ", "amend! ", 'Revert "', "Merge "):
            with self.subTest(prefix=prefix):
                self.assertEqual(self.run_hook(subject(90, prefix) + "\n").returncode, 0)

    def test_counts_characters_not_bytes(self):
        for env in ({"LC_ALL": "C"}, {"LC_ALL": "C.UTF-8"}):
            with self.subTest(env=env):
                for text in ("\u2192" * 71, "\u00fc" * 71):
                    self.assertEqual(self.run_hook(text + "\n", env).returncode, 0)
                self.assertEqual(self.run_hook("\u00fc" * 72 + "\n", env).returncode, 1)

    def test_trailing_whitespace_and_crlf_are_ignored(self):
        self.assertEqual(self.run_hook(subject(69) + "   \n").returncode, 0)
        self.assertEqual(self.run_hook(subject(71) + "\r\n\r\nbody\r\n").returncode, 0)
        self.assertEqual(self.run_hook(subject(72) + "  \n").returncode, 1)

    def test_leading_blank_lines_do_not_skip_the_check(self):
        self.assertEqual(self.run_hook("\n\n" + subject(80) + "\n").returncode, 1)
        self.assertEqual(self.run_hook("\n# c\n\n" + subject(10) + "\n").returncode, 0)

    def test_scissors_diff_is_not_measured(self):
        scissors = "# " + "-" * 24 + " >8 " + "-" * 24 + "\n"
        diff = "diff --git a/x b/x\n+" + "y" * 100 + "\n"
        self.assertEqual(self.run_hook("Short\n\n" + scissors + diff).returncode, 0)
        self.assertEqual(self.run_hook(subject(80) + "\n" + scissors + diff).returncode, 1)

    def test_empty_message_is_left_to_git(self):
        self.assertEqual(self.run_hook("# only a comment\n").returncode, 0)


class PrivacyCheckTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.repo = Path(tmp.name)
        (self.repo / "tools").mkdir()
        shutil.copy(PRIVACY_CHECK, self.repo / "tools")
        shutil.copytree(HOOKS, self.repo / "tools" / "githooks",
                        ignore=shutil.ignore_patterns("test_*", "__pycache__"))
        self.git("init", "-q", "-b", "main")
        self.git("config", "user.name", "Tester")
        self.git("config", "user.email", "tester@users.noreply.github.com")
        self.git("config", "commit.gpgsign", "false")
        self.git("commit", "-q", "--allow-empty", "-m", "Base")
        self.git("tag", "base")

    def git(self, *args):
        return subprocess.run(["git", *args], cwd=self.repo, check=True,
                              capture_output=True, text=True)

    def commit(self, message):
        self.git("commit", "-q", "--allow-empty", "-m", message)

    def check(self):
        return subprocess.run([SHELL, "tools/privacy-check.sh", "base"],
                              cwd=self.repo, capture_output=True, text=True)

    def test_71_passes_and_72_fails(self):
        self.commit(subject(71))
        self.assertEqual(self.check().returncode, 0, self.check().stderr)
        self.commit(subject(72))
        result = self.check()
        self.assertEqual(result.returncode, 1)
        self.assertIn("72 characters", result.stderr)

    def test_revert_and_merge_subjects_are_exempt(self):
        self.commit(subject(90, 'Revert "'))
        self.commit(subject(90, "Merge "))
        self.assertEqual(self.check().returncode, 0, self.check().stderr)

    def test_autosquash_subjects_are_rejected(self):
        for prefix in ("fixup! ", "squash! ", "amend! "):
            with self.subTest(prefix=prefix):
                self.git("reset", "-q", "--hard", "base")
                self.commit(prefix + "Short")
                result = self.check()
                self.assertEqual(result.returncode, 1)
                self.assertIn("squashed before merging", result.stderr)


if __name__ == "__main__":
    unittest.main()
