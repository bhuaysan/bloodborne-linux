#!/usr/bin/env python3
"""Write a candidate name list from the HLE library registrations of a shadPS4 checkout.

shadPS4 registers each reimplemented function as LIB_FUNCTION("<NID>", "<library>", <version>,
"<module>", <function>), so the function identifiers are names that someone paired with a NID.
They are only candidates: nidmatch accepts one only when it hashes to a NID of the pinned files.
A listed NID is not used, so a wrong pairing in the emulator cannot name a NID here.

Input: a shadPS4 checkout (default analysis/src/shadPS4).
Only src/core/libraries/ is read, and only the identifiers, which are facts. The C name behind
each posix_<name> identifier is a candidate too.

Output, one name per line, in analysis/nids/candidates/ (local, regenerate any time):
    shadps4-<first 7 characters of the checkout's commit>.txt
The file stem is the label in the `source` column of symbols/nids.csv, so it names the list and its
revision. The commit is also in docs/references.md.

Usage:
    shadps4names.py [CHECKOUT]
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "common"))
import nidmatch as nm  # noqa: E402
from toolenv import analysis_dir  # noqa: E402

DEFAULT_CHECKOUT = analysis_dir() / "src" / "shadPS4"
LIBRARIES = Path("src") / "core" / "libraries"
# LIB_FUNCTION("nid", "lib", 1, "module", ns::function); the call may span lines, and the kernel
# wraps the function as ORBIS(function).
REGISTER_RE = re.compile(
    r'LIB_FUNCTION\(\s*"[A-Za-z0-9+-]{11}"\s*,\s*"\w+"\s*,\s*\d+\s*,\s*"\w+"\s*,'
    r'\s*(?:ORBIS\(\s*)?(?:[A-Za-z_]\w*::)*([A-Za-z_]\w*)\s*\)')
# shadPS4 names its POSIX functions posix_<C name>; the C name is a candidate too.
POSIX_PREFIX = "posix_"


def registered_names(text: str) -> set[str]:
    """The function identifiers of the LIB_FUNCTION registrations in one source file, and the C
    names behind the posix_* ones."""
    names = set(REGISTER_RE.findall(text))
    return names | {n.removeprefix(POSIX_PREFIX) for n in names if n.startswith(POSIX_PREFIX)}


def checkout_names(root: Path) -> set[str]:
    base = root / LIBRARIES
    if not base.is_dir():
        raise nm.MatchError(f"{base} is not a directory, is {root} a shadPS4 checkout?")
    names: set[str] = set()
    for path in sorted(base.rglob("*.cpp")):
        names |= registered_names(path.read_text(encoding="utf-8", errors="replace"))
    return names


def revision(root: Path) -> str:
    run = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"], capture_output=True,
                         text=True)
    commit = run.stdout.strip()
    if run.returncode != 0 or not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise nm.MatchError(f"{root}: cannot read the commit (is it a git checkout?)")
    return commit


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("checkout", nargs="?", type=Path, default=DEFAULT_CHECKOUT,
                        help="shadPS4 checkout (default: %(default)s)")
    args = parser.parse_args(argv)
    try:
        commit = revision(args.checkout)
        names = checkout_names(args.checkout)
        if not names:
            raise nm.MatchError(f"{args.checkout / LIBRARIES}: no LIB_FUNCTION registrations")
        nm.GENERATED_DIR.mkdir(parents=True, exist_ok=True)
        label = f"shadps4-{commit[:7]}"
        nm.write_list(nm.GENERATED_DIR / f"{label}.txt",
                      [f"Function names of the LIB_FUNCTION registrations in shadPS4 @{commit},",
                       f"{LIBRARIES.as_posix()}/ (written by tools/nidmatch/shadps4names.py)"],
                      names)
    except (nm.MatchError, OSError) as exc:
        return nm.print_error(exc)
    return 0


if __name__ == "__main__":
    sys.exit(main())
