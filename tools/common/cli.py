"""Command-line helpers for the tools that read pinned files and write their outputs to a directory.

The pinned tables are passed in at call time, like for toolenv.read_pinned, so tests can replace
them in the calling tool.
"""

from __future__ import annotations

import argparse
import sys
from collections.abc import Callable
from pathlib import Path
from typing import TypeVar

from toolenv import GAME_BUILD_HINT, PinnedError, is_inside, load_game_dir, read_pinned

T = TypeVar("T")


def pinned_elf_paths(elf_dir: Path, pinned: dict[str, str]) -> list[Path]:
    """The pinned file names in `elf_dir`. Other files there are ignored."""
    return [elf_dir / name for name in sorted(set(pinned.values()))]


def default_elf_paths(parser: argparse.ArgumentParser, elf_dir: Path,
                      pinned: dict[str, str]) -> list[Path]:
    """pinned_elf_paths, for a run without file arguments. A usage error if elf_dir is missing."""
    if not elf_dir.is_dir():
        parser.error(f"no files given and {elf_dir} does not exist (run self2elf first)")
    return pinned_elf_paths(elf_dir, pinned)


def checked_out_dir(out_dir: Path, inputs: list[Path],
                    self2elf_dir: Path | None = None) -> Path | None:
    """`out_dir` resolved, or None after an error message if it lies in the game directory, the
    directory of an input or `self2elf_dir`.

    Checked before anything is written. self2elf_dir holds the self2elf outputs that the other
    tools verify against, so writing there would break later runs.
    """
    out_dir = out_dir.expanduser().resolve()
    forbidden = {p.resolve().parent for p in inputs}
    if self2elf_dir is not None:
        forbidden.add(self2elf_dir.resolve())
    game_dir = load_game_dir()
    if game_dir is not None:
        forbidden.add(game_dir)
    what = "game or input" if self2elf_dir is None else "game, input or self2elf"
    for parent in sorted(forbidden):
        if is_inside(out_dir, parent):
            print(f"error: refusing to write into the {what} directory ({parent})", file=sys.stderr)
            return None
    return out_dir


def convert_each(files: list[Path], pinned: dict[str, str], convert: Callable[[bytes], T],
                 write: Callable[[str, T], None], errors: tuple[type[Exception], ...],
                 hint: str = GAME_BUILD_HINT, label: Callable[[Path], str] = str) -> int:
    """Reads each file through read_pinned and calls write(pinned name, convert(contents)).

    A file that can't be read or converted (OSError, PinnedError or one of `errors`) gets an
    error message, and the batch goes on. Returns the exit status: 1 if any file failed, else 0.
    """
    failed = False
    for path in files:
        try:
            known, data = read_pinned(path, pinned, hint)
            result = convert(data)
        except (OSError, PinnedError, *errors) as exc:
            print(f"{label(path)}: error: {exc}", file=sys.stderr)
            failed = True
            continue
        write(known, result)
    return 1 if failed else 0
