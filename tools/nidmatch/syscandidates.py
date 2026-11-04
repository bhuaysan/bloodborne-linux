#!/usr/bin/env python3
"""Write candidate name lists from public sources installed on this Linux system.

Lists, each one name per line, in analysis/nids/candidates/ (local, regenerate any time):
    man2.txt, man3.txt     page names of the installed manual pages, sections 2 and 3
    glibc.txt              symbols defined by the system's libc.so.6 and libm.so.6
    libstdcxx.txt          symbols defined by the system's libstdc++.so.6 (mangled C++ names)

The names are only candidates. nidmatch accepts one only if it hashes to a NID of the game. The
header of each list records where it came from, and the label of a match is the list's stem.

By default the manual pages are found with manpath(1) and the libraries where the dynamic linker
finds them, so the tool works the same on any distribution.

Usage:
    syscandidates.py [--man-dir DIR ...] [--lib-dir DIR]
"""

from __future__ import annotations

import argparse
import ctypes
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "common"))
from toolenv import analysis_dir  # noqa: E402

OUT_DIR = analysis_dir() / "nids" / "candidates"
# Page files are named <name>.<section>[<suffix>][.<compression>], e.g. pthread_create.3.gz or
# BIO_read.3ssl.gz. Anything else in a man directory (README, index files) is not a page.
PAGE_RE = r"([A-Za-z_][A-Za-z0-9_]*)\.{section}[a-z0-9]*(?:\.(?:gz|bz2|xz|zst|lzma|Z))?"
LIBS = {"glibc": ["libc.so.6", "libm.so.6"], "libstdcxx": ["libstdc++.so.6"]}


def man_dirs() -> list[Path]:
    """The system's manual page directories, as man(1) itself searches them."""
    try:
        out = subprocess.run(["manpath"], capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError) as exc:
        raise OSError(f"manpath failed ({exc}), pass --man-dir") from exc
    return [Path(d) for d in out.strip().split(":") if d]


def man_names(man_dirs: list[Path], section: str) -> set[str]:
    page_re = re.compile(PAGE_RE.format(section=re.escape(section)))
    names = set()
    for man_dir in man_dirs:
        section_dir = man_dir / f"man{section}"
        if not section_dir.is_dir():
            continue
        for path in section_dir.iterdir():
            if m := page_re.fullmatch(path.name):
                names.add(m.group(1))
    return names


def find_library(soname: str) -> Path:
    """The file the dynamic linker loads for soname: load it here and look it up in our mappings."""
    ctypes.CDLL(soname)  # OSError if the linker does not find it
    for line in Path("/proc/self/maps").read_text().splitlines():
        fields = line.split(maxsplit=5)
        if len(fields) == 6:
            path = Path(fields[5])
            # The mapped file may be the versioned target of the link, e.g. libstdc++.so.6.0.33.
            if path.name == soname or path.name.startswith(soname + "."):
                return path
    raise OSError(f"{soname} is loaded but not in /proc/self/maps, pass --lib-dir")


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
    parser.add_argument("--man-dir", type=Path, action="append",
                        help="manual page directory (repeatable, default: from manpath)")
    parser.add_argument("--lib-dir", type=Path,
                        help="directory with the libraries (default: where the dynamic linker "
                             "finds them)")
    args = parser.parse_args(argv)

    try:
        OUT_DIR.mkdir(parents=True, exist_ok=True)
        dirs = args.man_dir or man_dirs()
        for section in ("2", "3"):
            write_list(OUT_DIR / f"man{section}.txt",
                       [f"Names of the installed manual pages, section {section}"],
                       man_names(dirs, section))
        for label, libs in LIBS.items():
            names: set[str] = set()
            for lib in libs:
                names |= defined_symbols(args.lib_dir / lib if args.lib_dir else find_library(lib))
            write_list(OUT_DIR / f"{label}.txt",
                       [f"Symbols defined by the system's {', '.join(libs)}"], names)
    except (OSError, subprocess.CalledProcessError, UnicodeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
