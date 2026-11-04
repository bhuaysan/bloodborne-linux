"""Shared helpers for the Python tools: repository paths, local.env and pinned files.

local.env and analysis/ are untracked, so they exist only in the main checkout. A linked git
worktree (`git worktree add`) has neither. main_checkout() finds the main checkout so that tools
and tests behave the same in both.
"""

from __future__ import annotations

import hashlib
import os
import subprocess
from functools import cache
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

@cache
def main_checkout() -> Path:
    """Root of the main checkout. Same as REPO_ROOT unless this is a linked worktree."""
    try:
        run = subprocess.run(["git", "rev-parse", "--path-format=absolute", "--git-common-dir"],
                             cwd=REPO_ROOT, capture_output=True, text=True, check=True)
    except (OSError, subprocess.CalledProcessError):
        return REPO_ROOT  # not a git checkout, e.g. an exported tarball
    common = Path(run.stdout.strip())
    return common.parent if common.name == ".git" else REPO_ROOT


def analysis_dir() -> Path:
    return main_checkout() / "analysis"


def local_env(key: str) -> str | None:
    """`key` from the environment, else from local.env in the main checkout."""
    value = os.environ.get(key)
    env_file = main_checkout() / "local.env"
    if not value and env_file.is_file():
        for line in env_file.read_text().splitlines():
            name, sep, rest = line.strip().partition("=")
            if sep and name.removeprefix("export ").strip() == key:
                value = rest.strip().strip("\"'")
    return value or None


def load_game_dir() -> Path | None:
    """BB_GAME_DIR from the environment, else from local.env in the main checkout."""
    value = local_env("BB_GAME_DIR")
    return Path(value).expanduser().resolve() if value else None


def is_inside(path: Path, parent: Path) -> bool:
    return path == parent or parent in path.parents


class PinnedError(Exception):
    pass


GAME_BUILD_HINT = "only CUSA03173 patch 1.09 is supported"


def read_pinned(path: Path, pinned: dict[str, str],
                hint: str = GAME_BUILD_HINT) -> tuple[str, bytes]:
    """Pinned name and contents of a file. `pinned` maps SHA-256 hex digests to names.

    Every tool reads its game-derived inputs through this, so all of them refuse other builds in
    the same way. Callers pass their own table at call time, which lets tests replace it. `hint`
    ends the error message and says how to get a pinned file.
    """
    data = path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    name = pinned.get(digest)
    if name is None:
        raise PinnedError(f"not a pinned file (sha256 {digest}); {hint}")
    return name, data
