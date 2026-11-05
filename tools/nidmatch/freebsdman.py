#!/usr/bin/env python3
"""Write candidate name lists from the FreeBSD 10.4-RELEASE manual page index.

Input: the man.freebsd.org apropos result pages for sections 2 and 3, saved as HTML, e.g.
    https://man.freebsd.org/cgi/man.cgi?query=.&apropos=1&sektion=2&manpath=FreeBSD+10.4-RELEASE&format=html
The query "." matches every page of the section. Each result links one page, and its link text
lists every name the page documents, e.g. "access(2), eaccess(2), faccessat(2)".

Output, one name per line, in analysis/nids/candidates/ (local, regenerate any time):
    freebsd-man2.txt, freebsd-man3.txt

The names are only candidates, like those of syscandidates.py. Section 3 also holds the pages of
third-party libraries shipped with FreeBSD (OpenSSL, Kerberos, libelf); a name still needs a
matching NID.

Usage:
    freebsdman.py SECTION2.html SECTION3.html
"""

from __future__ import annotations

import argparse
import html
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import nidmatch as nm  # noqa: E402

# One result: <a href="/cgi/man.cgi?query=access&amp;sektion=2&amp;...">access(2), eaccess(2)</a>
LINK_RE = re.compile(r'<a href="[^"]*[?&]query=[^"&]+&amp;sektion=(\w+)[^"]*">(.*?)</a>', re.S)
# A documented name with its section, e.g. "faccessat(2)" or "BIO_read(3)".
NAME_RE = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)\((\w+)\)")


def index_names(page: str, section: str) -> set[str]:
    """The names documented by the pages of one section that an apropos result page links."""
    names = set()
    for link in LINK_RE.finditer(page):
        if not link.group(1).startswith(section):
            continue
        text = html.unescape(re.sub(r"<[^>]+>", "", link.group(2)))
        for name, sect in NAME_RE.findall(text):
            if sect.startswith(section):
                names.add(name)
    return names


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("section2", type=Path, help="saved apropos page of section 2")
    parser.add_argument("section3", type=Path, help="saved apropos page of section 3")
    args = parser.parse_args(argv)

    try:
        nm.GENERATED_DIR.mkdir(parents=True, exist_ok=True)
        for section, path in (("2", args.section2), ("3", args.section3)):
            names = index_names(path.read_text(encoding="utf-8", errors="replace"), section)
            if not names:
                raise ValueError(f"{path}: no section {section} entries, is it an apropos page?")
            nm.write_list(nm.GENERATED_DIR / f"freebsd-man{section}.txt",
                          [f"Names of the FreeBSD 10.4-RELEASE manual pages, section {section}",
                           "(man.freebsd.org apropos index)"], names)
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
