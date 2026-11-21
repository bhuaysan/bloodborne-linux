#!/usr/bin/env python3
"""Tests for the release tools. Run: python3 -m unittest discover -s tools/release

Nothing here calls GitHub; the version tests make throwaway git repositories.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
import notes  # noqa: E402


def pr(number: int, branch: str, head: str, title: str = "") -> dict:
    return {"number": number, "headRefName": branch, "headRefOid": head,
            "title": title or f"PR {number}"}


class ReleasedPrs(unittest.TestCase):
    def test_keeps_the_prs_between_the_tags(self) -> None:
        history = {"v0.2.0": {"a", "b", "c"}, "v0.1.0": {"a"}}
        prs = [pr(3, "fix/x", "c"), pr(1, "feat/y", "a"), pr(2, "feat/z", "b"),
               pr(4, "fix/later", "d")]
        chosen = notes.released_prs(prs, "v0.2.0", "v0.1.0",
                                    ancestor=lambda commit, of: commit in history[of])
        self.assertEqual([p["number"] for p in chosen], [2, 3])


class Render(unittest.TestCase):
    def test_groups_by_branch(self) -> None:
        text = notes.render("## Highlights\n\n- Faster\n", [
            pr(5, "feat/fov", "a", "Add a field of view option"),
            pr(6, "fix/crash", "b", "Fix the crash at the lamp"),
            pr(7, "docs/rig", "c", "Record the rig"),
            pr(8, "feat/aniso", "d", "Add anisotropic filtering"),
        ], "owner/repo", "v0.2.0", "v0.1.0")
        self.assertEqual(text, (
            "## Highlights\n\n- Faster\n\n## Changes\n\n"
            "### Features\n\n- Add a field of view option (#5)\n- Add anisotropic filtering (#8)\n\n"
            "### Fixes\n\n- Fix the crash at the lamp (#6)\n\n"
            "### Other changes\n\n- Record the rig (#7)\n\n"
            "**Full changelog**: https://github.com/owner/repo/compare/v0.1.0...v0.2.0\n"))

    def test_first_release_has_no_list(self) -> None:
        text = notes.render("## Highlights\n", [], "owner/repo", "v0.1.0", None)
        self.assertEqual(text, "## Highlights\n\nThe first release.\n")


class Notes(unittest.TestCase):
    def test_rejects_other_tags(self) -> None:
        for tag in ("r11", "v1.2", "v01.2.3", "v1.2.3-rc1", "1.2.3"):
            with self.subTest(tag=tag), self.assertRaisesRegex(notes.NotesError, "not a release"):
                notes.notes(tag, "owner/repo")

    def test_needs_the_highlights(self) -> None:
        with self.assertRaisesRegex(notes.NotesError, "docs/releases/v99.0.0.md is missing"):
            notes.notes("v99.0.0", "owner/repo")


@unittest.skipUnless(shutil.which("cmake") and shutil.which("git"), "needs cmake and git")
class VersionScript(unittest.TestCase):
    """cmake/version.cmake, run as the build runs it."""

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)
        self.repo = self.tmp / "repo"
        self.repo.mkdir()
        self.env = dict(os.environ, GIT_CONFIG_GLOBAL="/dev/null", GIT_CONFIG_NOSYSTEM="1",
                        GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@example.invalid",
                        GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@example.invalid")
        self.git("init", "-q")
        self.commit("one")

    def git(self, *args: str) -> str:
        return subprocess.run(["git", *args], cwd=self.repo, env=self.env, check=True,
                              capture_output=True, text=True).stdout.strip()

    def commit(self, text: str) -> None:
        (self.repo / "file").write_text(text)
        self.git("add", "file")
        self.git("commit", "-q", "-m", text)

    def version(self, source: Path | None = None, override: str = "") -> str:
        out = self.tmp / "version.cpp"
        subprocess.run(["cmake", f"-DSOURCE_DIR={source or self.repo}", f"-DOUTPUT={out}",
                        f"-DVERSION={override}", "-P", str(ROOT / "cmake" / "version.cmake")],
                       env=self.env, check=True, capture_output=True)
        match = re.search(r'return "([^"]*)";', out.read_text())
        assert match
        return match.group(1)

    def test_versions(self) -> None:
        head = self.git("rev-parse", "--short", "HEAD")
        self.assertEqual(self.version(), f"dev-{head}")
        self.git("tag", "r11")  # milestone tags don't count
        self.assertEqual(self.version(), f"dev-{head}")
        self.git("tag", "v0.1.0")
        self.assertEqual(self.version(), "v0.1.0")
        self.commit("two")
        self.assertRegex(self.version(), r"^v0\.1\.0-1-g[0-9a-f]+$")
        (self.repo / "file").write_text("changed")
        self.assertRegex(self.version(), r"^v0\.1\.0-1-g[0-9a-f]+-dirty$")
        self.assertEqual(self.version(override="v9.9.9"), "v9.9.9")

    def test_without_git(self) -> None:
        plain = self.tmp / "plain"
        plain.mkdir()
        self.assertEqual(self.version(source=plain), "unknown")

    def test_unchanged_version_keeps_the_file(self) -> None:
        self.version()
        out = self.tmp / "version.cpp"
        before = out.stat().st_mtime_ns
        os.utime(out, ns=(before - 10**9, before - 10**9))
        self.version()
        self.assertEqual(out.stat().st_mtime_ns, before - 10**9)


if __name__ == "__main__":
    unittest.main()
