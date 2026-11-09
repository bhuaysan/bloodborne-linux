#!/usr/bin/env python3
"""Reproduce the bulk import of shadPS4's shader_recompiler (R3, issue #415).

Fetches shadPS4 at the pinned release and the submodules it records there, and writes:

    src/shadps4/                    shader_recompiler and the part of common/ and video_core/
                                    it needs, changed only by add_notice() and rewrite_includes()
    third_party/shadps4/LICENSES/   the license texts those files refer to by SPDX tag
    third_party/<dependency>/       unchanged copies, with their license files

The set of files is the measured dependency closure of src/shader_recompiler: an #include walk
for shadPS4's own files, and the preprocessor over every translated source for the third-party
headers (boost-headers.txt). Files outside the closure, or under a license that is not allowed
by docs/source-policy.md, stay out. HOST_PROVIDED lists what the host layer writes instead.

Usage: import_shadps4.py --cache DIR --write    regenerate the tree in this checkout
       import_shadps4.py --cache DIR --check    compare the tree with a fresh generation

DIR holds the fetched repositories and may be reused. It must be outside the tracked tree.
"""

from __future__ import annotations

import argparse
import filecmp
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
TOOL_DIR = Path(__file__).resolve().parent

SHADPS4_URL = "https://github.com/shadps4-emu/shadPS4.git"
SHADPS4_COMMIT = "11557f2eb60b3b00712c8887c30852de7615d6df"  # tag v.0.21.0
NOTICE_DATE = "2026-11-09"

COMMON_FILES = """
arch.h assert.h bit_field.h concepts.h decoder.cpp decoder.h div_ceil.h enum.h func_traits.h
hash.h io_file.h logging/classes.cpp logging/classes.h logging/log.h object_pool.h path_util.h
signal_context.cpp signal_context.h singleton.h small_vector.h types.h
""".split()

AMDGPU_FILES = """
pixel_format.cpp pixel_format.h regs_shader.h regs_vertex.h resource.h tiling.cpp tiling.h
""".split()

# Quoted includes of the imported files that deliberately have no imported target. Issue #416
# provides them. common/alignment.h is CC0-1.0, which is not on the allow-list; the others are
# emulator code that would pull in the rest of the emulator.
HOST_PROVIDED = {
    "common/alignment.h",
    "core/emulator_settings.h",
    "core/libraries/kernel/process.h",
    "core/signals.h",
}

# SPDX identifiers from the allow-list in docs/source-policy.md.
ALLOWED_LICENSES = {
    "GPL-2.0-or-later",
    "LGPL-2.1-only",
    "LGPL-2.1-or-later",
    "MIT",
    "BSD-2-Clause",
    "BSD-3-Clause",
    "ISC",
    "Zlib",
    "BSL-1.0",
    "MPL-2.0",
}

LICENSE_TEXTS = ["GPL-2.0-or-later.txt", "MIT.txt", "BSD-3-Clause.txt"]


@dataclass(frozen=True)
class Dependency:
    name: str  # directory under third_party/
    submodule: str  # path of the gitlink in shadPS4
    url: str
    license_file: str
    files: tuple[str, ...]  # files or directories, relative to the repository


def boost_headers() -> tuple[str, ...]:
    lines = (TOOL_DIR / "boost-headers.txt").read_text().splitlines()
    return tuple(line for line in lines if line and not line.startswith("#"))


def dependencies() -> list[Dependency]:
    return [
        Dependency("sirit", "externals/sirit", "https://github.com/shadps4-emu/sirit.git",
                   "LICENSE.txt", ("include", "src")),
        Dependency("boost", "externals/ext-boost", "https://github.com/shadps4-emu/ext-boost.git",
                   "LICENSE_1_0.txt", boost_headers()),
        Dependency("fmt", "externals/fmt", "https://github.com/shadps4-emu/ext-fmt.git", "LICENSE",
                   ("include/fmt/base.h", "include/fmt/format-inl.h", "include/fmt/format.h")),
        Dependency("magic_enum", "externals/magic_enum", "https://github.com/Neargye/magic_enum.git",
                   "LICENSE", ("include/magic_enum/magic_enum.hpp",)),
        Dependency("xbyak", "externals/xbyak", "https://github.com/herumi/xbyak.git", "COPYRIGHT",
                   ("xbyak/xbyak.h", "xbyak/xbyak_mnemonic.h", "xbyak/xbyak_util.h")),
    ]


# Roots this script owns. --check reports anything else found below them.
def owned_roots() -> list[str]:
    return ["src/shadps4", "third_party/shadps4"] + [f"third_party/{d.name}" for d in dependencies()]


# Include directories of the imported tree, for the closure check.
INCLUDE_DIRS = ["src/shadps4", "third_party/sirit/include", "third_party/boost",
                "third_party/fmt/include", "third_party/magic_enum/include", "third_party/xbyak"]


class ImportError_(Exception):
    pass


def git(*args: str, cwd: Path) -> str:
    result = subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True, text=True)
    return result.stdout.strip()


def fetch(url: str, commit: str, directory: Path) -> Path:
    """Check out one commit of a repository, reusing an earlier fetch."""
    if not (directory / ".git").exists():
        directory.mkdir(parents=True, exist_ok=True)
        git("init", "-q", cwd=directory)
        git("remote", "add", "origin", url, cwd=directory)
    try:
        current = git("rev-parse", "HEAD", cwd=directory)
    except subprocess.CalledProcessError:
        current = ""
    if current != commit:
        git("fetch", "-q", "--depth", "1", "origin", commit, cwd=directory)
        git("checkout", "-q", "--detach", "FETCH_HEAD", cwd=directory)
    if git("rev-parse", "HEAD", cwd=directory) != commit:
        raise ImportError_(f"{url}: expected commit {commit}")
    return directory


def submodule_commit(shadps4: Path, path: str) -> str:
    entry = git("ls-tree", SHADPS4_COMMIT, path, cwd=shadps4).split()
    if len(entry) < 3 or entry[1] != "commit":
        raise ImportError_(f"{path} is not a submodule of shadPS4 at the pinned commit")
    return entry[2]


def spdx_expression(text: str) -> str | None:
    match = re.search(r"SPDX-License-Identifier:\s*([^\r\n]+)", text)
    return match.group(1).strip() if match else None


def license_allowed(expression: str) -> bool:
    """True if the expression can be used under the allow-list.

    AND needs every term allowed; OR needs one. Mixed expressions don't occur here and are
    refused, so that a person looks at them.
    """
    if " AND " in expression and " OR " in expression:
        return False
    if " OR " in expression:
        return any(term.strip() in ALLOWED_LICENSES for term in expression.split(" OR "))
    return all(term.strip() in ALLOWED_LICENSES for term in expression.split(" AND "))


def add_notice(text: str, source_path: str) -> str:
    """Insert the Source and Changed lines after the file's leading SPDX lines."""
    newline = "\r\n" if "\r\n" in text else "\n"
    lines = text.split(newline)
    last = -1
    for index, line in enumerate(lines[:10]):
        if line.lstrip("﻿").startswith("// SPDX-"):
            last = index
    if last < 0:
        raise ImportError_(f"{source_path}: no leading SPDX lines")
    notice = [
        f"// Source: shadPS4 src/{source_path} @{SHADPS4_COMMIT}",
        f"// Changed for bloodborne-linux, {NOTICE_DATE}",
        "// License texts: third_party/shadps4/LICENSES/",
    ]
    return newline.join(lines[: last + 1] + notice + lines[last + 1 :])


def rewrite_includes(text: str) -> str:
    """Make the one include that names shadPS4's src/ directory relative to the include root."""
    return text.replace('#include "src/common/types.h"', '#include "common/types.h"')


def write(destination: Path, data: bytes) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(data)


def import_shadps4_file(source: Path, relative: str, output: Path) -> None:
    text = source.read_bytes().decode("utf-8")
    expression = spdx_expression(text)
    if expression is None:
        raise ImportError_(f"{relative}: no SPDX license identifier")
    if not license_allowed(expression):
        raise ImportError_(f"{relative}: license {expression!r} is not on the allow-list")
    write(output / "src/shadps4" / relative, rewrite_includes(add_notice(text, relative)).encode("utf-8"))


def generate(cache: Path, output: Path) -> None:
    shadps4 = fetch(SHADPS4_URL, SHADPS4_COMMIT, cache / "shadPS4")
    src = shadps4 / "src"

    translator = sorted(p for p in (src / "shader_recompiler").rglob("*") if p.is_file())
    relatives = [str(p.relative_to(src)) for p in translator]
    relatives += [f"common/{name}" for name in COMMON_FILES]
    relatives += [f"video_core/amdgpu/{name}" for name in AMDGPU_FILES]
    for relative in relatives:
        import_shadps4_file(src / relative, relative, output)

    for name in LICENSE_TEXTS:
        write(output / "third_party/shadps4/LICENSES" / name, (shadps4 / "LICENSES" / name).read_bytes())

    for dep in dependencies():
        repo = fetch(dep.url, submodule_commit(shadps4, dep.submodule), cache / dep.name)
        destination = output / "third_party" / dep.name
        write(destination / dep.license_file, (repo / dep.license_file).read_bytes())
        for item in dep.files:
            path = repo / item
            if path.is_dir():
                for file in sorted(p for p in path.rglob("*") if p.is_file()):
                    write(destination / file.relative_to(repo), file.read_bytes())
            else:
                write(destination / item, path.read_bytes())

    check_closure(output)


def check_closure(output: Path) -> None:
    """Every quoted include must resolve inside the import, or be listed in HOST_PROVIDED."""
    include = re.compile(r'^\s*#\s*include\s*"([^"]+)"', re.M)
    roots = [output / d for d in INCLUDE_DIRS]
    used_host: set[str] = set()
    missing: list[str] = []
    tree = output / "src/shadps4"
    for path in sorted(p for p in tree.rglob("*") if p.is_file()):
        for name in include.findall(path.read_text(encoding="utf-8")):
            if name in HOST_PROVIDED:
                used_host.add(name)
            elif not any((root / name).is_file() for root in [path.parent, *roots]):
                missing.append(f"{path.relative_to(tree)}: {name}")
    if missing:
        raise ImportError_("includes outside the import:\n  " + "\n  ".join(missing))
    stale = HOST_PROVIDED - used_host
    if stale:
        raise ImportError_(f"HOST_PROVIDED entries no longer used: {sorted(stale)}")


def files_below(root: Path, owned: list[str]) -> set[str]:
    found: set[str] = set()
    for name in owned:
        base = root / name
        if base.is_dir():
            found.update(str(p.relative_to(root)) for p in base.rglob("*") if p.is_file())
    return found


def compare(generated: Path, checkout: Path) -> list[str]:
    owned = owned_roots()
    have = files_below(checkout, owned)
    want = files_below(generated, owned)
    problems = [f"missing: {name}" for name in sorted(want - have)]
    problems += [f"not generated: {name}" for name in sorted(have - want)]
    problems += [
        f"differs: {name}"
        for name in sorted(want & have)
        if not filecmp.cmp(generated / name, checkout / name, shallow=False)
    ]
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cache", type=Path, required=True, help="directory for the fetched repositories")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--write", action="store_true", help="regenerate the tree in this checkout")
    mode.add_argument("--check", action="store_true", help="compare the tree with a fresh generation")
    args = parser.parse_args()

    try:
        with tempfile.TemporaryDirectory() as temp:
            generated = Path(temp)
            generate(args.cache.resolve(), generated)
            if args.check:
                problems = compare(generated, REPO_ROOT)
                for problem in problems:
                    print(problem)
                print(f"{len(problems)} difference(s)")
                return 1 if problems else 0
            for name in owned_roots():
                shutil.rmtree(REPO_ROOT / name, ignore_errors=True)
                shutil.copytree(generated / name, REPO_ROOT / name)
            print(f"wrote {len(files_below(REPO_ROOT, owned_roots()))} files")
            return 0
    except (ImportError_, subprocess.CalledProcessError) as error:
        print(f"import_shadps4: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
