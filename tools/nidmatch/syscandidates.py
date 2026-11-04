#!/usr/bin/env python3
"""Write candidate name lists from public sources installed on this Linux system.

Lists, each one name per line, in analysis/nids/candidates/ (local, regenerate any time):
    man2.txt, man3.txt     page names of the installed manual pages, sections 2 and 3
    glibc.txt              symbols defined by the system's libc.so.6 and libm.so.6
    libstdcxx.txt          symbols defined by the system's libstdc++.so.6 (mangled C++ names)

The names are only candidates. nidmatch accepts one only if it hashes to a NID of the game. The
header of each list records where it came from, and the label of a match is the list's stem.

Usage:
    syscandidates.py [--man-dir DIR] [--lib-dir DIR]
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "common"))
from toolenv import analysis_dir  # noqa: E402

OUT_DIR = analysis_dir() / "nids" / "candidates"
IDENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
LIBS = {"glibc": ["libc.so.6", "libm.so.6"], "libstdcxx": ["libstdc++.so.6"]}


def man_names(man_dir: Path, section: str) -> set[str]:
    # Page files are named <name>.<section>[<suffix>][.gz], e.g. pthread_create.3.gz, BIO_read.3ssl.gz
    names = set()
    for path in (man_dir / f"man{section}").iterdir():
        name = path.name.split(f".{section}", 1)[0]
        if IDENT_RE.fullmatch(name):
            names.add(name)
    return names


def defined_symbols(lib: Path) -> set[str]:
    out = subprocess.run(["nm", "-D", "--defined-only", "--format=posix", str(lib)],
                         capture_output=True, text=True, check=True).stdout
    names = set()
    for line in out.splitlines():
        name = line.split(" ", 1)[0].split("@", 1)[0]  # drop symbol versions, e.g. memcpy@@GLIBC_2.14
        if name:
            names.add(name)
    return names


def write_list(path: Path, header: list[str], names: set[str]) -> None:
    lines = [f"# {h}" for h in header] + sorted(names)
    path.write_text("\n".join(lines) + "\n", encoding="ascii")
    print(f"{path.name}: {len(names)} names")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--man-dir", type=Path, default=Path("/usr/share/man"))
    parser.add_argument("--lib-dir", type=Path, default=Path("/usr/lib64"))
    args = parser.parse_args(argv)

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    try:
        for section in ("2", "3"):
            write_list(OUT_DIR / f"man{section}.txt",
                       [f"Names of the installed manual pages, section {section}"],
                       man_names(args.man_dir, section))
        for label, libs in LIBS.items():
            names: set[str] = set()
            for lib in libs:
                names |= defined_symbols(args.lib_dir / lib)
            write_list(OUT_DIR / f"{label}.txt",
                       [f"Symbols defined by the system's {', '.join(libs)}"], names)
    except (OSError, subprocess.CalledProcessError, UnicodeEncodeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
