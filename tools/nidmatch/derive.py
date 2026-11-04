#!/usr/bin/env python3
"""Derive candidate names for sce* NIDs from names we already have.

These are the derivation rules allowed by the NID section of docs/clean-room.md. Each rule writes
one list to analysis/nids/candidates/ (local, regenerate any time):
    derive-posix.txt   <prefix><C name in CamelCase>   sce + pthread_attr_setstacksize
    derive-words.txt   <prefix><1 to 3 words>          sceFios + FH + Open + Sync

A library's prefixes come from its name, word by word: libSceNpMatching2 gives sceNp, sceNpMatching2
and, without the trailing digits, sceNpMatching. libkernel gives only the bare sce, as in its
import scePthreadAttrSetstacksize. Libraries in nidmatch.NO_DERIVE_LIBRARIES get no prefixes, their
NIDs are never targets (not even when another library shares them), their names give no words,
and no derived name may start with one of their prefixes.

The words:
    sce words   CamelCase words of the matched sce* names; up to 3 per candidate
    all words   the sce words, the parts of the C names, and the CamelCase words that occur in at
                least 2 identifiers in the pinned ELFs; up to 2 per candidate

A candidate is kept only if its NID is a NID of a library its prefix came from. Kept names add their
words to the vocabulary, and the rules run again until nothing new turns up. So the lists hold only
names that match, and nidmatch records them with the list's stem as the label. See
docs/formats/nid.md for the matches that motivate each rule.

Input:
    symbols/nids.csv             matched names (rows whose only sources are derive-* are ignored)
    analysis/elf/*.elf           the pinned ELFs, loaded as for nidmatch: their NIDs per library
                                 and their identifiers
    analysis/nids/candidates/    glibc.txt, man2.txt, man3.txt from syscandidates.py

Usage:
    derive.py [--jobs N]
"""

from __future__ import annotations

import argparse
import hashlib
import itertools
import os
import re
import sys
import time
from collections import Counter, defaultdict
from multiprocessing import Pool
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "common"))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "elfimports"))
import elfimports  # noqa: E402
import nidmatch as nm  # noqa: E402
from nid import NID_SUFFIX, nid_digest  # noqa: E402

POSIX_LABEL = "derive-posix"
WORDS_LABEL = "derive-words"
C_LISTS = ("glibc", "man2", "man3")  # written by syscandidates.py
SCE_DEPTH = 3
ALL_DEPTH = 2
MIN_IDENTIFIERS = 2  # a word from the ELFs must occur in this many distinct identifiers

# A CamelCase word: an acronym (FH in sceFiosFHOpen), a capitalised word, or a lower-case run.
WORD_RE = re.compile(r"[A-Z]+(?![a-z])|[A-Z][a-z0-9]*|[a-z0-9]+")
C_NAME_RE = re.compile(r"_*[a-z][a-z0-9_]*")
CAMEL_RE = re.compile(r"[a-z][A-Z]")

# prefix -> NID key -> libraries
Targets = dict[str, dict[bytes, tuple[str, ...]]]


def cap(word: str) -> str:
    return word[:1].upper() + word[1:]


def words(identifier: str) -> list[str]:
    return [cap(w) for part in identifier.split("_") for w in WORD_RE.findall(part)]


def camel(c_name: str) -> str:
    return "".join(cap(part) for part in c_name.split("_") if part)


def blocked_prefixes() -> tuple[str, ...]:
    return tuple("sce" + lib.removeprefix("libSce") for lib in sorted(nm.NO_DERIVE_LIBRARIES))


def prefixes(library: str) -> set[str]:
    if library in nm.NO_DERIVE_LIBRARIES:
        return set()
    if library == "libkernel":
        # Our only libkernel match with a prefix, scePthreadAttrSetstacksize, has the bare sce. No
        # match shows a prefix made from the name libkernel itself.
        return {"sce"}
    if not library.startswith("libSce"):
        return set()
    ws = WORD_RE.findall(library.removeprefix("libSce"))
    out = {"sce" + "".join(ws[:i]) for i in range(1, len(ws) + 1)}
    out |= {p.rstrip("0123456789") for p in out}
    return out - {"sce"}


def targets(table: nm.NidTable) -> Targets:
    out: dict[str, dict[bytes, list[str]]] = defaultdict(lambda: defaultdict(list))
    for h, libs in table.items():
        if nm.NO_DERIVE_LIBRARIES & libs.keys():
            continue  # nidmatch.check_derived would refuse a derived name for it
        for lib in libs:
            for prefix in prefixes(lib):
                out[prefix][nid_digest(h)].append(lib)
    return {p: {k: tuple(sorted(v)) for k, v in keys.items()} for p, keys in out.items()}


def new_combos(old: list[bytes], new: list[bytes], k: int):
    """Every k-tuple of words from old + new with at least one new word, each exactly once: the
    first new word sits at position j, words before it are old, words after it are any."""
    full = old + new
    for j in range(k):
        yield from itertools.product(*([old] * j + [new] + [full] * (k - j - 1)))


# The targets of the current process. Set once per worker, so jobs don't carry them.
_targets: Targets = {}


def set_targets(tgt: Targets) -> None:
    global _targets
    _targets = tgt


Job = tuple[str, list[bytes], list[bytes], int]


def search(job: Job) -> list[str]:
    """Names <prefix><up to depth words> whose NID is a target, trying only combinations that
    contain a new word."""
    prefix, old, new, depth = job
    keys = _targets[prefix]
    base = hashlib.sha1(prefix.encode("ascii"))
    found = []
    for k in range(1, depth + 1):
        for combo in new_combos(old, new, k):
            h = base.copy()
            h.update(b"".join(combo) + NID_SUFFIX)
            if h.digest()[:8] in keys:
                found.append(prefix + b"".join(combo).decode("ascii"))
    return found


class Searcher:
    """Runs the searches, in one pool of worker processes for its whole life (jobs > 1)."""

    def __init__(self, tgt: Targets, jobs: int):
        self.prefixes = sorted(tgt)
        self.blocked = blocked_prefixes()
        set_targets(tgt)
        self.pool = Pool(jobs, initializer=set_targets, initargs=(tgt,)) if jobs > 1 else None

    def __enter__(self) -> Searcher:
        return self

    def __exit__(self, *exc) -> None:
        if self.pool:
            self.pool.terminate()
            self.pool.join()

    def run(self, old: list[str], new: list[str], depth: int) -> set[str]:
        if not new:
            return set()
        o = [w.encode("ascii") for w in old]
        n = [w.encode("ascii") for w in new]
        work = [(p, o, n, depth) for p in self.prefixes]
        parts = self.pool.map(search, work, chunksize=1) if self.pool else map(search, work)
        return {name for part in parts for name in part if not name.startswith(self.blocked)}


def sce_words(names: set[str]) -> set[str]:
    return {w for n in names if n.startswith("sce") for w in words(n[3:])}


def identifier_words(idents: set[str]) -> set[str]:
    counts = Counter(w for ident in idents if CAMEL_RE.search(ident) for w in set(words(ident)))
    return {w for w, c in counts.items() if c >= MIN_IDENTIFIERS and len(w) >= 2}


def derive(tgt: Targets, seed: set[str], c_names: set[str], idents: set[str],
           jobs: int = 1, log=lambda _: None) -> dict[str, set[str]]:
    """label -> derived names that match. seed: names matched by non-derived sources."""
    c_names = {c for c in c_names if C_NAME_RE.fullmatch(c)}
    with Searcher(tgt, jobs) as searcher:
        posix = searcher.run([], sorted({camel(c) for c in c_names}), 1)
        log(f"{POSIX_LABEL}: {len(posix)} names")

        fixed = {w for c in c_names for w in words(c)} | identifier_words(idents)
        known = seed | posix
        found: set[str] = set()
        tried_sce: list[str] = []
        tried_all: list[str] = []
        for round_no in itertools.count(1):
            sce = sce_words(known)
            new_sce = sorted(sce - set(tried_sce))
            new_all = sorted((sce | fixed) - set(tried_all))
            if not new_sce and not new_all:
                break
            hits = (searcher.run(tried_sce, new_sce, SCE_DEPTH)
                    | searcher.run(tried_all, new_all, ALL_DEPTH))
            tried_sce += new_sce
            tried_all += new_all
            found |= hits
            known |= hits
            log(f"{WORDS_LABEL} round {round_no}: {len(new_sce)} new sce words, "
                f"{len(new_all)} new words, {len(found)} names")
    return {POSIX_LABEL: posix, WORDS_LABEL: found}


def read_seed(path: Path) -> set[str]:
    """Names in nids.csv with at least one source that is not a derivation rule. Names of the
    libraries in NO_DERIVE_LIBRARIES are left out, so their words never feed a rule."""
    names: set[str] = set()
    blocked: set[str] = set()
    for _, row in nm.nids_csv_rows(path):
        if row["library"] in nm.NO_DERIVE_LIBRARIES:
            blocked.add(row["name"])
        elif any(not s.startswith(nm.DERIVED_PREFIX) for s in row["source"].split(";") if s):
            names.add(row["name"])
    return names - blocked


def read_c_names(cand_dir: Path) -> set[str]:
    names: nm.Candidates = {}
    for stem in C_LISTS:
        path = cand_dir / f"{stem}.txt"
        if not path.is_file():
            raise nm.MatchError(f"{path} is missing (run syscandidates.py first)")
        nm.read_candidate_file(path, names)
    return set(names)


def read_identifiers(elfs: dict[str, bytes]) -> set[str]:
    return {t.decode("ascii") for data in elfs.values() for t in nm.IDENT_RE.findall(data)}


def write_list(path: Path, rule: str, names: set[str]) -> None:
    header = [f"# {path.stem}: {rule}",
              "# Written by tools/nidmatch/derive.py. Only names whose NID matched."]
    path.write_text("\n".join(header + sorted(names)) + "\n", encoding="ascii")
    print(f"{path.name}: {len(names)} names")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1,
                        help="worker processes (default: one per CPU)")
    args = parser.parse_args(argv)

    start = time.monotonic()
    try:
        infos, elfs = elfimports.load_dir(nm.ELF_DIR)
        table = nm.load_nids(infos)
        idents = read_identifiers(elfs)
        c_names = read_c_names(nm.GENERATED_DIR)
        seed = read_seed(nm.NIDS_CSV)
        result = derive(targets(table), seed, c_names, idents, max(1, args.jobs),
                        log=lambda msg: print(f"[{time.monotonic() - start:4.0f}s] {msg}"))
        write_list(nm.GENERATED_DIR / f"{POSIX_LABEL}.txt",
                   "<prefix><C name in CamelCase>", result[POSIX_LABEL])
        write_list(nm.GENERATED_DIR / f"{WORDS_LABEL}.txt",
                   "<prefix><1 to 3 words>", result[WORDS_LABEL])
    except nm.ERRORS as exc:
        return nm.print_error(exc)
    return 0


if __name__ == "__main__":
    sys.exit(main())
